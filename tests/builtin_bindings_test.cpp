// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/screens/screen.h>
#include <o3os/core/binding.h>
#include <o3os/core/keymap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::Context;

namespace {

// Every context of the app and the word a keymap line uses for it (bindings.cpp): the test checks the two lists agree.
struct ContextWord {
  const char* word;
  Context context;
};
constexpr ContextWord kContextWords[] = {
    {"global", Context::Global},
    {"pattern", Context::Pattern},
    {"pattern.edit", Context::PatternEdit},
    {"pattern.follow", Context::PatternFollow},
    {"pattern.select", Context::PatternSelect},
    {"pattern.browse", Context::PatternBrowse},
    {"pattern.note", Context::PatternNote},
    {"pattern.instr", Context::PatternInstr},
    {"pattern.vol", Context::PatternVol},
    {"pattern.fx", Context::PatternFx},
    {"chooser", Context::Chooser},
    {"file", Context::File},
    {"sampleload", Context::SampleLoad},
    {"wave", Context::Wave},
    {"wave.chop", Context::WaveChop},
    {"wave.wt", Context::WaveWt},
    {"wave.gran", Context::WaveGran},
    {"playmode", Context::PlayModePicker},
    {"verbmenu", Context::VerbMenu},  // OS-PTOOLS2 S3
    {"patterntools", Context::PatternTools},  // OS-PTOOLS2 S2: the Pattern TOOLS dialog over the block
    {"events", Context::Events},  // OS-MIDIREC S1b
    {"learnmap", Context::LearnMap},  // OS-LEARNMAP
    {"instrument.empty", Context::InstrumentEmpty},
    {"file.export", Context::FileExport},
    {"exportprogress", Context::ExportProgress},
    {"help", Context::Help},
    {"sampleedit", Context::SampleEdit},
    {"sampleedit.fx", Context::SampleEditFx},
    {"sampleedit.plug", Context::SampleEditPlug},
    {"config", Context::Config},
    {"mixer", Context::Mixer},
    {"mixer.stems", Context::MixerStems},
    {"mixer.inserts", Context::MixerInserts},  // OS-INSKEYS
    {"instrument.insert", Context::InsertParams},  // OS-INSKEYS
    {"perform", Context::Perform},
    {"perform.dj", Context::PerformDj},
    {"record", Context::Record},
    {"song", Context::Song},
    {"learn", Context::Learn},
    {"navigator", Context::Navigator},
    {"inputtest", Context::InputTest},
    {"instrument", Context::Instrument},
    {"modules", Context::Modules},
    {"moduleapp", Context::ModuleApp},
    {"modulepicker", Context::ModulePicker},
    {"preset", Context::Preset},
    {"textentry", Context::TextEntry},
    {"steppopup", Context::StepPopup},
    {"pads.tracks", Context::PadsTracks},
    {"pads.mode", Context::PadsMode},
    {"padspicker", Context::PadsPicker},  // OS-UXF-PAT 7
    {"fill", Context::FillDialog},
    {"song.ask", Context::SongAsk},      // OS-SONGMAP
    {"song.tools", Context::SongTools},
    {"firmware", Context::FirmwareUpdate},  // OS-FWCONFIG
    {"record.radio", Context::RecordRadio},
};

const char* context_word(core::ContextId id) {
  for (const ContextWord& c : kContextWords) {
    if (app::context_id(c.context) == id) {
      return c.word;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("builtin bindings: the built-in layer has no conflicts and names only registered actions") {
  const core::BindingView builtin = app::builtin_bindings();
  CHECK(builtin.count >= 50u);
  CHECK_FALSE(core::find_conflict(builtin.items, builtin.count).found);
  for (std::size_t i = 0; i < builtin.count; ++i) {
    const core::Binding& b = builtin.items[i];
    CHECK_MESSAGE(app::find_action(app::action_registry(), b.action) != nullptr, "binding " << i);
    CHECK_MESSAGE(context_word(b.context) != nullptr, "binding " << i << " is in a context with no name");
  }
}

TEST_CASE("builtin bindings: contexts, the modifier set and the pool sizes") {
  CHECK(app::context_id(Context::Global) == 0);
  CHECK(app::context_id(Context::Pattern) == 1);
  // The numbers are stable (a keymap names contexts, but the chain and the tables carry them): never renumber.
  CHECK(app::context_id(Context::PatternEdit) == 2);
  CHECK(app::context_id(Context::PatternFollow) == 3);
  CHECK(app::context_id(Context::PatternSelect) == 4);
  CHECK(app::context_id(Context::Song) == 5);
  CHECK(app::context_id(Context::Learn) == 6);
  CHECK(app::context_id(Context::Navigator) == 7);
  CHECK(app::context_id(Context::InputTest) == 8);
  CHECK(app::context_id(Context::Instrument) == 9);
  CHECK(app::context_id(Context::PatternBrowse) == 10);  // OS-009: the fourth mode, the four fields, the chooser strip
  CHECK(app::context_id(Context::PatternNote) == 11);
  CHECK(app::context_id(Context::PatternInstr) == 12);
  CHECK(app::context_id(Context::PatternVol) == 13);
  CHECK(app::context_id(Context::PatternFx) == 14);
  CHECK(app::context_id(Context::Chooser) == 15);
  CHECK(app::context_id(Context::File) == 16);  // OS-010a: the File screen
  CHECK(app::context_id(Context::SampleLoad) == 17);  // OS-011a: the Sample load screen
  CHECK(app::context_id(Context::Wave) == 18);        // OS-011a: the Wave / Chop screen and its CHOP page
  CHECK(app::context_id(Context::WaveChop) == 19);
  CHECK(app::context_id(Context::SampleEdit) == 20);  // OS-011a: the Sample edit screen
  CHECK(app::context_id(Context::Config) == 21);      // OS-011a: the Config screen
  CHECK(app::context_id(Context::FirmwareUpdate) == 48);  // OS-FWCONFIG: the firmware update dialog (46 and 47 went to the DJ page and Help; 41-43 are left to the branches that add contexts)
  CHECK(app::context_id(Context::Record) == 27);      // OS-011c: the Record screen (22, 23 Mixer and Perform; 24-26 the modules' contexts)
  CHECK(app::kModifierButtons == (std::uint64_t{1} << static_cast<unsigned>(Button::Shift)));
  CHECK(app::keymap_names().modifiers == (app::kModifierButtons | (std::uint64_t{1} << static_cast<unsigned>(Button::Note)) | (std::uint64_t{1} << static_cast<unsigned>(Button::Copy)) | (std::uint64_t{1} << static_cast<unsigned>(Button::F7)) | (std::uint64_t{1} << static_cast<unsigned>(Button::Master))));  // the loader names Shift (the recogniser's own modifier), Note, Copy, F7 and Master (held modifiers where the bindings say so: OS-ZOOM, OS-SONGSHARE S3, OS-PTOOLS2 S3b, OS-MIXINSHOLD)
  CHECK(sizeof(core::Binding) == 16u);
  CHECK(app::kUserBindings * sizeof(core::Binding) + app::kPresetBindings * sizeof(core::Binding) <= 2048u);
}

TEST_CASE("builtin bindings: the keymap names resolve every button, both contexts and every action") {
  const core::KeymapNames& names = app::keymap_names();
  for (const app::ButtonName& b : app::kButtonNames) {
    std::uint8_t id = 0xFF;
    CHECK_MESSAGE(names.button(names.ctx, b.name, id), b.name);
    CHECK(id == static_cast<std::uint8_t>(b.button));
  }
  core::ContextId ctx = 0xFF;
  for (const ContextWord& c : kContextWords) {
    ctx = 0xFF;
    CHECK_MESSAGE(names.context(names.ctx, c.word, ctx), c.word);
    CHECK(ctx == app::context_id(c.context));
  }
  CHECK_FALSE(names.context(names.ctx, "nowhere", ctx));
  CHECK_FALSE(names.context(names.ctx, "pattern.", ctx));
  std::uint8_t id = 0;
  CHECK_FALSE(names.button(names.ctx, "nobutton", id));
  CHECK_FALSE(names.button(names.ctx, "", id));
  const app::ActionTable t = app::action_registry();
  for (std::size_t i = 0; i < t.count; ++i) {
    core::ActionId action = core::ActionId::None;
    CHECK_MESSAGE(names.action(names.ctx, t.items[i].name, action), t.items[i].name);
    CHECK(action == t.items[i].id);
  }
  core::ActionId action = core::ActionId::None;
  CHECK_FALSE(names.action(names.ctx, "no.such", action));
}

TEST_CASE("builtin bindings: through the app's names only Shift may modify") {
  const auto load = [](const std::string& text) {
    core::BindingPool<8> pool;
    return pool.load(text.data(), text.size(), app::keymap_names()).error;
  };
  CHECK(load("o3keymap 1\npattern tap shift+left nav.down\n") == core::KeymapError::None);
  CHECK(load("o3keymap 1\npattern tap vol+left nav.down\n") == core::KeymapError::BadCombination);
  CHECK(load("o3keymap 1\npattern tap rec+play nav.down\n") == core::KeymapError::BadCombination);
  CHECK(load("o3keymap 1\npattern chord rec+play nav.down\n") == core::KeymapError::None);
}

// The built-in layer written as a keymap: the text format can say everything the constexpr table says, and says
// it the same way (so a preset can be a copy of the built-ins with changes). The text is generated from the table,
// binding by binding, in the keymap's own words, so a new built-in binding is covered the day it is added.
namespace {

std::string button_word(std::uint8_t id) {
  for (const app::ButtonName& b : app::kButtonNames) {
    if (static_cast<std::uint8_t>(b.button) == id) {
      return b.name;
    }
  }
  return "?";
}

std::string input_word(core::InputRef in) {
  switch (in.kind) {
    case core::InputKind::Button:
      return button_word(in.id);
    case core::InputKind::Pad:
      return in.id == core::kAnyId ? "pad*" : "pad" + std::to_string(in.id);
    case core::InputKind::Encoder:
      return "enc" + std::to_string(in.id);
    default:
      break;
  }
  return "?";
}

std::string binding_line(const core::Binding& b) {
  const core::Trigger& t = b.trigger;
  const char* gesture = "?";
  std::string trigger;
  switch (t.gesture) {
    case core::GestureKind::Tap:
      gesture = "tap";
      break;
    case core::GestureKind::Hold:
      gesture = "hold";
      break;
    case core::GestureKind::HoldEnd:
      gesture = "holdend";
      break;
    case core::GestureKind::DoubleTap:
      gesture = "double";
      break;
    case core::GestureKind::HoldEncoder:
      gesture = "holdenc";
      break;
    case core::GestureKind::Chord:
      gesture = "chord";
      break;
  }
  // A modifier (or a held button, or the lesser chord member) comes first, joined with '+'.
  trigger = core::is_none(t.modifier) ? input_word(t.input) : input_word(t.modifier) + "+" + input_word(t.input);
  const core::ActionId id = b.action;
  const app::ActionDef* a = app::find_action(app::action_registry(), id);
  std::string arg;
  const int offset = b.arg;
  switch (b.arg_from) {
    case core::ArgFrom::Fixed:
      arg = " " + std::to_string(offset);
      break;
    case core::ArgFrom::InputId:
      arg = " @id" + (offset == 0 ? std::string() : (offset > 0 ? "+" : "") + std::to_string(offset));
      break;
    case core::ArgFrom::Value:
      arg = " @value" + (offset == 0 ? std::string() : (offset > 0 ? "+" : "") + std::to_string(offset));
      break;
  }
  return std::string(context_word(b.context)) + " " + gesture + " " + trigger + " " + (a != nullptr ? a->name : "none") + arg + "\n";
}

}  // namespace

TEST_CASE("builtin bindings: the built-in table written as keymap text parses to the same bindings") {
  const core::BindingView builtin = app::builtin_bindings();
  std::string text = "o3keymap 1\n";
  for (std::size_t i = 0; i < builtin.count; ++i) {
    text += binding_line(builtin.items[i]);
  }
  core::BindingPool<1024> pool;  // the whole built-in table (515 after OS-SCREENSHOT and APPUX 3.1, more with the app page's screen buttons; test-only capacity)
  const core::KeymapResult r = pool.load(text.data(), text.size(), app::keymap_names());
  CHECK_MESSAGE(r.ok(), core::keymap_error_text(r.error) << " at line " << r.line << ": " << text);
  CHECK(pool.size() == builtin.count);
  for (std::size_t i = 0; i < pool.size() && i < builtin.count; ++i) {
    CHECK_MESSAGE(pool.view().items[i] == builtin.items[i], "binding " << i << ": " << binding_line(builtin.items[i]));
  }
  // The text really covers every gesture the table uses (a table of taps alone would prove little).
  for (const char* word : {" tap ", " hold ", " holdend ", " chord ", "@value", "@id+1"}) {
    CHECK_MESSAGE(text.find(word) != std::string::npos, word);
  }
}

// ---- the Shift combinations: the conflict check of OS-002b -----------------------------------------------------------------
//
// Shift is the one declared modifier. Every built-in binding that names it is listed here, so a new one is a decision made
// in this file: none of them may conflict with another in its context (the table's static_assert) or hide a plain key.

namespace {

struct Chain {
  const char* what;
  std::vector<Context> contexts;  // innermost first; the first one captures when `captures` is set
  bool captures;
};

core::ContextChain chain_of(const Chain& c) {
  core::ContextChain chain;
  for (std::size_t i = 0; i < c.contexts.size(); ++i) {
    CHECK(chain.push(app::context_id(c.contexts[i]), c.captures && i == 0));
  }
  return chain;
}

core::Resolution resolve_in(const Chain& c, core::GestureKind kind, Button button, bool shift) {
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const core::Gesture g{0, kind, app::button_ref(button), shift ? app::button_ref(Button::Shift) : core::kNoInput, 0};
  return core::resolve(set, chain_of(c), g);
}

}  // namespace

TEST_CASE("builtin bindings: every Shift binding is one of these, and none of them hides a plain key") {
  const core::BindingView builtin = app::builtin_bindings();
  std::vector<std::string> shifted;
  for (std::size_t i = 0; i < builtin.count; ++i) {
    const core::Binding& b = builtin.items[i];
    if (b.trigger.modifier == app::button_ref(Button::Shift)) {
      CHECK(b.trigger.gesture != core::GestureKind::Chord);  // a chord's "modifier" is its lesser member, never Shift here
      shifted.push_back(binding_line(b));
    }
  }
  std::vector<std::string> expected;
  const auto line = [](const char* context, const char* gesture, const char* input, const char* action, int arg) {
    return std::string(context) + " " + gesture + " shift+" + input + " " + action + " " + std::to_string(arg) + "\n";
  };
  expected.push_back(line("global", "tap", "master", "ui.theme.next", 0));
  expected.push_back(line("global", "tap", "rec", "pattern.rec_free", 0));  // OS-RECSWAP: Edit following / free (the plain Rec enters Edit free); it was the mode cycle until the Select mode went
  expected.push_back(line("global", "tap", "pattern", "apps.open", 0));  // OS-APPSNAV: the list of the apps that are not tools (the Navigator is the chords Master + Song and Song + Pattern; OS-NAV2 had it here)
  expected.push_back(line("global", "tap", "file", "file.save_any", 0));  // OS-UXF-KEYS 1a: SAVE from any screen (File's F4; no folder yet: the save-as keyboard)
  expected.push_back(line("global", "tap", "config", "help.open", 0));  // OS-HELP: the help page of the screen in view; the plain Config is the Config screen
  expected.push_back(line("moduleapp", "tap", "config", "help.open", 0));  // (the app page captures, so Global's chord does not reach it)
  expected.push_back(line("pattern", "tap", "f8", "edit.redo", 0));  // OS-002c: F8 is UNDO on Pattern and Song, Shift + F8 REDO
  expected.push_back(line("song", "tap", "f8", "edit.redo", 0));
  // OS-SONG1: the Song screen's editing: Shift + Insert (OS-SHIFTROW; it was Shift + F3) the slot with a copy of the pattern, Shift + Up / Down the range, Shift + encoder the tempo's tenths, Shift + Copy the paste.
  expected.push_back("song tap shift+insert song.add_slot 1\n");
  expected.push_back("song tap shift+up song.select -1\n");
  expected.push_back("song tap shift+down song.select 1\n");
  expected.push_back("song tap shift+enc0 song.turn_fine @value\n");
  expected.push_back("song tap shift+copy song.paste 0\n");
  expected.push_back("song tap shift+left song.select_tracks -1\n");  // OS-SONGMAP: the range of tracks, in the map
  expected.push_back("song tap shift+right song.select_tracks 1\n");
  expected.push_back("song tap shift+delete song.cut 0\n");  // OS-SONGSHARE S2: CUT (it fell back to the plain Delete before)
  expected.push_back("song tap shift+note song.next_use -1\n");  // OS-SONGSHARE S4: the previous slot that holds the cursor's pattern (the plain Note is the next one)
  expected.push_back(line("instrument", "tap", "f8", "edit.redo", 0));  // OS-002d: the parameter page
  expected.push_back("instrument tap shift+enc0 param.coarse @value\n");  // Shift + encoder: the coarse step (the binding line has no fixed argument)
  expected.push_back(line("instrument", "tap", "left", "param.coarse", -1));  // OS-INSTUI-ARROWS: Shift + Left / Right are the coarse step of the row
  expected.push_back(line("instrument", "tap", "right", "param.coarse", 1));
  expected.push_back(line("config", "tap", "left", "param.coarse", -1));  // OS-CFG-ARROWS: Shift + Left / Right are the coarse step of the row on Config and Record
  expected.push_back(line("config", "tap", "right", "param.coarse", 1));
  expected.push_back(line("record", "tap", "left", "record.coarse", -1));
  expected.push_back(line("record", "tap", "right", "record.coarse", 1));
  expected.push_back(line("instrument.empty", "tap", "left", "instrument.turn", -1));  // the empty slot's chooser: Shift + arrow is the plain arrow (no value to step)
  expected.push_back(line("instrument.empty", "tap", "right", "instrument.turn", 1));
  // OS-INSTUI S1, S2: Shift + F1..F5 are NAME .. SAVE (placeholders until S5 / S7), Shift + F6 / F7 the nearest instrument in use.
  // OS-SHIFTPAD: Shift + pad plays the instrument in view like a plain pad (it no longer selects).
  expected.push_back("instrument tap shift+pad* instrument.pad @id\n");
  {
    static constexpr const char* kShiftRow[5] = {"instrument.name", "instrument.copy", "instrument.paste", "instrument.delete", "instrument.save"};  // OS-INSTUI S5, S7
    for (int n = 0; n < 5; ++n) {
      expected.push_back(line("instrument", "tap", (std::string("f") + std::to_string(n + 1)).c_str(), kShiftRow[n], 0));
    }
  }
  expected.push_back(line("instrument", "tap", "delete", "instrument.init", 0));  // Shift + Delete INIT
  expected.push_back(line("instrument", "tap", "f6", "instrument.used", -1));
  expected.push_back(line("instrument", "tap", "f7", "instrument.used", 1));
  expected.push_back(line("instrument.insert", "tap", "f6", "ui.none", 0));  // OS-INSKEYS: an insert's parameter page has no neighbouring instrument: Shift + F6 / F7 do nothing
  expected.push_back(line("instrument.insert", "tap", "f7", "ui.none", 0));
  // OS-009 P1a.1: the block, HOME and pull, the octave of the encoder, mute and solo on the first two rows of pads.
  expected.push_back("pattern tap shift+left select.extend -10\n");
  expected.push_back("pattern tap shift+right select.extend 10\n");
  expected.push_back("pattern tap shift+up select.extend -1\n");
  expected.push_back("pattern tap shift+down select.extend 1\n");
  expected.push_back("pattern tap shift+insert cursor.jump_fraction 0\n");
  expected.push_back("pattern tap shift+delete step.pull 0\n");
  expected.push_back("pattern tap shift+copy block.paste 0\n");  // OS-PATCOPY (Copy alone is block.copy)
  expected.push_back("pattern.note tap shift+enc0 step.note_octave @value\n");
  expected.push_back("pattern.browse tap shift+enc0 cursor.page @value\n");  // OS-SHIFTENC: Browse pages under Shift (it scrolled one row, as the plain turn, until then; Follow, which had the same rows, is retired: OS-004d)
  // OS-SHIFTENC: Shift + encoder, a coarse step where a value or a list had no Shift meaning: the Instr / Vol / FX value columns (x8 / x10 / x16), a page in the lists, the coarse step of the numeric overlays.
  expected.push_back("pattern.instr tap shift+enc0 step.instrument_coarse @value\n");
  expected.push_back("pattern.vol tap shift+enc0 step.vol_coarse @value\n");
  expected.push_back("pattern.fx tap shift+enc0 step.fx_value_coarse @value\n");
  for (const char* list : {"file", "sampleload", "playmode", "learn", "navigator", "modules", "modulepicker", "preset", "verbmenu", "help"}) {
    expected.push_back(std::string(list) + " tap shift+enc0 cursor.page @value\n");
  }
  expected.push_back("steppopup tap shift+enc0 popup.page @value\n");
  expected.push_back("file.export tap shift+enc0 export.coarse @value\n");
  expected.push_back("chooser tap shift+enc0 chooser.coarse @value\n");
  expected.push_back("chooser tap shift+copy pattern.clip_paste 0\n");  // PAT-OPS-B: with F1 held, Shift + Copy writes the whole clipboard pattern over the chooser's pattern (Copy alone is pattern.clip_copy)
  expected.push_back("fill tap shift+enc0 fill.coarse @value\n");
  // OS-PADS-A / OS-PADOCT: Shift + F1 cycles the pads' octave; OS-UXF-PAT 7: Shift + F2 opens the PADS picker (its F1..F6 pick the mode: NOTES 0, PIANO 1, ISO 3, TRACKS 2, PATS 5, VU 6; F3..F7 of the Shift row are free);
  // Shift + pad mute and solo are gone (the TRACKS layer has the pads). STEP (4) has no key (OS-STEPPADOFF).
  expected.push_back(line("pattern", "tap", "f1", "pads.octave", 0));
  expected.push_back(line("pattern", "tap", "f2", "pads.picker", 0));
  // OS-QJUMP: Shift + Note, Instr, Vol (FX1) and Fx (FX2) are the quarter jumps (Shift + Fx was FILL again; pattern.fill has no key now, the F4 dialog is the Fill key).
  expected.push_back(line("pattern", "tap", "note", "select.jump", 0));  // OS-SELSHIFT: in Edit they also select the rows they cross
  expected.push_back(line("pattern", "tap", "instr", "select.jump", 1));
  expected.push_back(line("pattern", "tap", "vol", "select.jump", 2));
  expected.push_back(line("pattern", "tap", "fx", "select.jump", 3));
  expected.push_back(line("pattern", "tap", "play", "transport.play_cursor", 0));  // OS-PLAYCUR: play from the cursor's row (also the Focus Loop's key)
  // OS-011a: the Wave / Chop screen: the coarse encoder, Shift + pad selects a slice silently, F8 REDO as on the other editors, and on the CHOP page
  // Shift + Up / Down set how many slices EVEN makes.
  expected.push_back("wave tap shift+enc0 wave.nudge_coarse @value\n");
  expected.push_back("wave tap shift+pad* wave.pad_select @id\n");
  expected.push_back(line("wave", "tap", "f5", "edit.redo", 0));  // OS-UXF-SMP item 11: REDO moved from F8 to F5; F7 / F8 are INST - / INST + on every sample screen
  expected.push_back(line("wave", "tap", "f7", "sedit.instrument", -1));
  expected.push_back(line("wave", "tap", "f8", "sedit.instrument", 1));
  expected.push_back(line("wave", "tap", "f1", "wave.mode", 0));  // OS-INSTUI S4: MODE, the list of the eight play modes
  expected.push_back(line("wave", "tap", "f6", "preview.hold", static_cast<int>(app::Button::F6) | 0x100));  // OS-FU3: Shift + PREVIEW plays from where the screen is
  expected.push_back("wave.chop tap shift+f4 wave.to_pat 0\n");  // OS-UXF-SMP 3c: TO PAT, the slices as notes on consecutive steps
  expected.push_back("wave.chop tap shift+up wave.even_n 1\n");
  expected.push_back("wave.chop tap shift+down wave.even_n -1\n");
  // The Sample edit screen: the coarse encoder, Shift + F6 saves the sample as a WAV (OS-007n: the key row is full), REDO on F8, Shift + Up / Down set Amplify's percentage.
  expected.push_back("sampleedit tap shift+enc0 sedit.move_coarse @value\n");
  expected.push_back(line("sampleedit", "tap", "f1", "sedit.amiga", 0));  // OS-AMG1: the AMIGA conversion at 8,287 / 16,574 Hz and its 8-bit option
  expected.push_back(line("sampleedit", "tap", "f2", "sedit.amiga", 1));
  expected.push_back(line("sampleedit", "tap", "f3", "sedit.q8", 0));
  expected.push_back(line("sampleedit", "tap", "f4", "sedit.run", 7));  // OS-SEDFX: DC REMOVE (EditOp::DcRemove)
  expected.push_back(line("sampleedit", "tap", "f7", "sedit.instrument", -1));  // OS-QAFIX-27 (NEXT INS on F5), OS-UXF-SMP item 11: INST - / INST +, round
  expected.push_back(line("sampleedit", "tap", "f8", "sedit.instrument", 1));
  expected.push_back(line("sampleedit", "tap", "f6", "sedit.save_wav", 0));  // (OS-FU3: from Shift + F7, which plays from the selection's edge)
  expected.push_back(line("sampleedit", "tap", "play", "preview.hold", static_cast<int>(app::Button::Play) | 0x100));  // OS-UXF-SMP item 11: PRV EDGE moved from Shift + F7 to Shift + Play
  expected.push_back(line("sampleedit", "tap", "f5", "sedit.redo", 0));  // moved from Shift + F8
  expected.push_back("sampleedit.fx tap shift+enc0 sedit.fx_coarse @value\n");  // OS-AUDIT-15: the FX page's coarse step (its plain encoder is sedit.fx)
  expected.push_back("sampleedit.fx tap shift+up ui.none 0\n");  // OS-AUDIT-15: the edit page's AMP is not on the FX page
  expected.push_back("sampleedit.fx tap shift+down ui.none 0\n");
  expected.push_back("sampleedit.plug tap shift+enc0 sedit.plug_coarse @value\n");  // OS-CEPTFX S4a: the PLUGINS page (the plain encoder is sedit.plug)
  expected.push_back("sampleedit.plug tap shift+up ui.none 0\n");  // the edit page's AMP is not on the PLUGINS page
  expected.push_back("sampleedit.plug tap shift+down ui.none 0\n");
  expected.push_back("sampleedit tap shift+up sedit.amp 1\n");
  expected.push_back("sampleedit tap shift+down sedit.amp -1\n");
  expected.push_back("wave.wt tap shift+enc0 param.coarse @value\n");    // OS-INSTUI S4: the WAVETABLE and GRANULAR views edit their rows as the source page does
  expected.push_back("wave.gran tap shift+enc0 param.coarse @value\n");
  expected.push_back("config tap shift+enc0 param.coarse @value\n");  // OS-011a: Config: the coarse step, as on PARAMS
  expected.push_back("mixer tap shift+delete mixer.reset 1\n");  // OS-REVIEW: STEMS: the factory map (the plain Delete there clears the cursor's source)
  expected.push_back("mixer tap shift+enc0 mixer.coarse @value\n");  // OS-011b: the Mixer: the coarse step
  // OS-011b: the Perform screen: the encoder is the held value under Shift too, Shift + Left / Right swap the slot in EDIT, Shift + Copy pastes it (Shift + F1..F8 are free since OS-PADS-A).
  expected.push_back("perform tap shift+enc0 perform.turn @value\n");
  expected.push_back("perform tap shift+left perform.swap -1\n");
  expected.push_back("perform tap shift+right perform.swap 1\n");
  expected.push_back("perform tap shift+copy perform.paste 0\n");
  // OS-DJ A1: the DJ page of Perform (its field level): Shift + encoder is the fine crossfader.
  expected.push_back("perform.dj tap shift+enc0 dj.fine @value\n");
  expected.push_back("perform.dj tap shift+left dj.tempo -1\n");  // OS-DJ A4: the fine tempo, 0.1 BPM a press
  expected.push_back("perform.dj tap shift+right dj.tempo 1\n");
  // OS-012b: an app's page keeps the Shift layer of its parameters (the app gets everything else): the row, the edit and the default.
  expected.push_back("moduleapp tap shift+f8 ui.none 0\n");  // OS-KEYROWDLG S5: Shift + F8 reads nothing and must not fall through to BACK
  expected.push_back("preset tap shift+f5 preset.save 0\n");  // OS-MODPRESET P4: SAVE in the F5 PRESET list (its own key, not a fall-through)
  expected.push_back("moduleapp tap shift+up nav.up 0\n");
  expected.push_back("moduleapp tap shift+down nav.down 0\n");
  expected.push_back("moduleapp tap shift+enc0 module.app_edit @value\n");
  expected.push_back("moduleapp tap shift+delete module.app_reset 0\n");
  expected.push_back("record tap shift+enc0 record.coarse @value\n");  // OS-011c: the Record screen: the coarse step
  // OS-SHIFTROW: Shift + F belongs to the screen. Global binds all eight to the null action (the keymap's `none`); the quarter jumps are bound in Song, File and Sample load.
  for (int n = 0; n < 8; ++n) {
    expected.push_back(line("global", "tap", (std::string("f") + std::to_string(n + 1)).c_str(), "ui.none", 0));
  }
  for (const char* context : {"song", "file", "sampleload"}) {
    for (int n = 0; n < 4; ++n) {
      expected.push_back(line(context, "tap", (std::string("f") + std::to_string(n + 1)).c_str(), "cursor.jump_fraction", n));
    }
    expected.push_back(line(context, "tap", "f5", "cursor.jump_end", 0));
  }
  expected.push_back("sampleload tap shift+f6 sample.mark 0\n");  // OS-UXF-SMP item 18: MARK
  expected.push_back("sampleload tap shift+f7 sample.target -1\n");  // OS-SLDTARGET: INST - / INST + on Sample load
  expected.push_back("sampleload tap shift+f8 sample.target 1\n");
  expected.push_back("sampleload tap shift+right sample.mark 0\n");  // OS-AUDIT-A7 (SLD-MULTI): mark the file and step down / clear the marks
  expected.push_back("sampleload tap shift+left sample.mark 1\n");
  // OS-NAV3: Shift + Up / Down is a page on the lists (cursor.page, the rows the list shows; the step popup's own popup.page).
  for (const char* context : {"file", "sampleload", "modules", "modulepicker", "preset", "verbmenu", "learn", "help"}) {  // (OS-H3A: the TOOLS list; OS-PTOOLS2 S3: the verb menu; OS-HELP: a screenful)
    expected.push_back(std::string(context) + " tap shift+up cursor.page -1\n");
    expected.push_back(std::string(context) + " tap shift+down cursor.page 1\n");
  }
  expected.push_back("song.tools tap shift+enc0 songtools.coarse @value\n");  // OS-SONGMAP: the TOOLS dialog's coarse step
  expected.push_back("patterntools tap shift+enc0 ptool.coarse @value\n");     // OS-PTOOLS2 S2: the Pattern TOOLS dialog's coarse step (an octave on AMOUNT)
  // OS-MIDIREC S1b/S2b/S2c/S3: the EVENTS overlay: a page on Shift + Up / Down, QUANT ALL on Shift + F2 (the argument 1 says every
  // row; F2 alone is the row's), Shift + Insert (the other kind, A9), REDO on Shift + F7 (events.redo: the list's own, which projects
  // again), and the coarse field edit on Shift + the encoder (S2b; the plain encoder is events.edit, not a Shift binding).
  expected.push_back("events tap shift+up events.page -1\n");
  expected.push_back("events tap shift+down events.page 1\n");
  expected.push_back("events tap shift+enc0 events.coarse @value\n");
  expected.push_back(line("events", "tap", "f2", "events.quant", 1));
  expected.push_back("events tap shift+insert events.insert 1\n");
  expected.push_back(line("events", "tap", "f7", "events.redo", 0));
  expected.push_back("steppopup tap shift+up popup.page -1\n");
  expected.push_back("steppopup tap shift+down popup.page 1\n");
  // OS-LEARNMAP: the LEARN MAP overlay: Shift + Up / Down a page of rows, Shift + encoder the coarse step.
  expected.push_back("learnmap tap shift+up cursor.page -1\n");
  expected.push_back("learnmap tap shift+down cursor.page 1\n");
  expected.push_back("learnmap tap shift+enc0 lmap.coarse @value\n");
  // The Navigator captures, so it has its own: no jumps; Shift + F1 / F8 stay GO / BACK (a Shift press falls back to the plain tap; OS-UXF-KEYS item 5), the rest nothing.
  for (int n = 0; n < 7; ++n) {
    expected.push_back(line("navigator", "tap", (std::string("f") + std::to_string(n + 1)).c_str(), n == 0 ? "navigator.accept" : "ui.none", 0));
  }
  expected.push_back(line("navigator", "tap", "f8", "ui.back", 0));
  expected.push_back(line("mixer", "tap", "f7", "mixer.mute_none", 0));  // OS-SHIFTROW: UNMUTE / UNSOLO every strip
  expected.push_back(line("mixer", "tap", "f8", "mixer.solo_none", 0));
  expected.push_back(line("mixer", "tap", "f1", "mixer.name", 0));  // OS-NAMES: NAME, the track under the cursor
  expected.push_back(line("song", "tap", "f6", "song.name", 0));    // the pattern of the slot
  expected.push_back(line("song", "tap", "f7", "song.pads", 0));    // OS-SONGPADS: the pads' TRACKS / SLOTS
  expected.push_back(line("file", "tap", "f6", "file.name", 0));    // the project's title
  for (int n = 0; n < 8; ++n) {  // STORE 1..3, then E-PERF-07's PICK -, PICK +, TRK PAT, TRK NEXT, TRK SONG
    expected.push_back(line("perform", "tap", (std::string("f") + std::to_string(n + 1)).c_str(), "perform.key", 0x100 + n));
  }
  expected.push_back(line("config", "tap", "f7", "config.page_default", 0));
  expected.push_back(line("firmware", "tap", "f1", "ui.none", 0));  // OS-FWCONFIG: the update dialog's two questions are answered by F1 alone
  expected.push_back(line("record", "tap", "f3", "record.crop", 0));  // OS-RECCROP: CROP, the take under review becomes its window (it asks first)
  expected.push_back(line("record.radio", "tap", "f5", "record.seek", -10));
  expected.push_back(line("record.radio", "tap", "f6", "record.seek", 10));
  expected.push_back(line("file", "tap", "f7", "file.purge", 0));        // OS-FILES part B: PURGE UNUSED on the PROJECTS page ...
  expected.push_back(line("file.export", "tap", "f7", "ui.none", 0));   // ... which the EXPORT page's field context takes back
  std::sort(shifted.begin(), shifted.end());
  std::sort(expected.begin(), expected.end());
  CHECK(shifted.size() == expected.size());
  for (std::size_t i = 0; i < shifted.size() && i < expected.size(); ++i) {
    CHECK_MESSAGE(shifted[i] == expected[i], "got " << shifted[i] << " expected " << expected[i]);
  }

  // Resolved in the chains they live in: Shift + F1..F4 jump to 0..3/4, Shift + F5 to the end (on Pattern F1 is pads.octave and F2..F5 are the pads' mode keys: pads.mode 0, 1, 3, 2, OS-PADS-A, OS-PADOCT), and plain F1..F5 are
  // untouched (the pattern and song F-keys still say their labels, the Navigator's F1 still opens).
  const Chain pattern{"pattern edit", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain select{"pattern select", {Context::PatternSelect, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain navigator{"navigator", {Context::Navigator, Context::Global}, true};
  const Button keys[5] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5};
  for (const Chain* c : {&pattern, &select, &song, &navigator}) {
    for (int n = 0; n < 5; ++n) {
      const core::Resolution with = resolve_in(*c, core::GestureKind::Tap, keys[n], true);
      const bool pads = c == &pattern || c == &select;
      if (pads && n == 0) {  // OS-PADOCT: Shift + F1 is the octave
        CHECK_MESSAGE((with.status == core::Resolution::Status::Bound && with.action == core::action_id("pads.octave")), c->what << " Shift+F1");
      } else if (pads && n == 1) {  // OS-UXF-PAT 7: Shift + F2 is the PADS picker
        CHECK_MESSAGE((with.status == core::Resolution::Status::Bound && with.action == core::action_id("pads.picker")), c->what << " Shift+F2");
      } else if (pads) {  // Shift + F3..F5 are free on Pattern: Global's null action
        CHECK_MESSAGE((with.status == core::Resolution::Status::Bound && with.action == core::action_id("ui.none")), c->what << " Shift+F" << n + 1);
      } else if (c == &navigator) {  // OS-SHIFTROW: no jumps in the Navigator; Shift + F2..F5 are nothing (bound to the null action); Shift + F1 is GO as the plain F1 (OS-UXF-KEYS item 5)
        CHECK_MESSAGE((with.status == core::Resolution::Status::Bound && with.action == core::action_id(n == 0 ? "navigator.accept" : "ui.none")), "navigator Shift+F" << n + 1);
      } else {
        CHECK_MESSAGE((with.status == core::Resolution::Status::Bound &&
                       with.action == core::action_id(n < 4 ? "cursor.jump_fraction" : "cursor.jump_end") && (n >= 4 || with.arg == n)),
                      c->what << " Shift+F" << n + 1);
      }
      const core::Resolution plain = resolve_in(*c, core::GestureKind::Tap, keys[n], false);
      if (c == &navigator && n != 0) {
        CHECK_MESSAGE(plain.status == core::Resolution::Status::Captured, c->what << " F" << n + 1);  // swallowed
      } else {
        // OS-009: on the pattern F1..F3 are the choosers and F5 is PREVIEW (reserved); F4 is FILL (OS-PTOOLS).
        const bool on_pattern = c == &pattern || c == &select;
        // OS-005e: on the song F2 is DUP PAT (OS-UXF-PAT 12), the action of Shift + Insert; the Play button plays the song from the selected row there.
        const char* name = c == &navigator ? "navigator.accept"
                           : on_pattern && n < 3 ? "chooser.toggle"
                           : on_pattern && n == 3 ? "fill.open"  // OS-PTOOLS: F4 FILL
                           : on_pattern && n == 4 ? "pattern.preview"
                           : on_pattern && n == 5 ? "events.open"    // OS-F6EVENTS: F6 EVENTS (RE FILL / REPEAT moved to the verb menu)
                           : c == &song ? (n == 0 ? "song.play_pattern" : n == 1 ? "song.add_slot" : n == 2 ? "song.add_slot" : n == 3 ? "song.delete_slot" : n == 4 ? "song.unique" : "song.tools")  // OS-SONG1; OS-SONGMAP: F5 UNIQUE, F6 TOOLS (they were the slot's pattern - / +)
                                                  : "ui.softkey";
        const core::ActionId want = core::action_id(name);
        CHECK_MESSAGE((plain.status == core::Resolution::Status::Bound && plain.action == want), c->what << " F" << n + 1);
      }
    }
  }
  // The parameter page (OS-002d, OS-INSTUI S1, S5, S7): Shift + F1..F5 are the Shift row (NAME, COPY, PASTE, DELETE, SAVE), never the plain key; plain F1..F5 are instrument.page.
  const Chain instrument{"instrument", {Context::Instrument, Context::Global}, false};
  for (int n = 0; n < 5; ++n) {
    const core::Resolution with = resolve_in(instrument, core::GestureKind::Tap, keys[n], true);
    static constexpr const char* kShiftRow[5] = {"instrument.name", "instrument.copy", "instrument.paste", "instrument.delete", "instrument.save"};
    CHECK_MESSAGE((with.status == core::Resolution::Status::Bound && with.action == core::action_id(kShiftRow[n]) && with.arg == 0), "instrument Shift+F" << n + 1);
    const core::Resolution plain = resolve_in(instrument, core::GestureKind::Tap, keys[n], false);
    CHECK_MESSAGE((plain.status == core::Resolution::Status::Bound && plain.action == core::action_id("instrument.page") && plain.arg == n), "instrument F" << n + 1);
  }
  // OS-INSTUI-ARROWS: Left / Right are the encoder's detent on the row, Shift + Left / Right its coarse step; the empty slot's chooser keeps the page turn (instrument.turn).
  const Chain chooser{"instrument chooser", {Context::InstrumentEmpty, Context::Instrument, Context::Global}, false};
  for (int dir = -1; dir <= 1; dir += 2) {
    const Button arrow = dir < 0 ? Button::Left : Button::Right;
    const core::Resolution edit = resolve_in(instrument, core::GestureKind::Tap, arrow, false);
    CHECK_MESSAGE((edit.status == core::Resolution::Status::Bound && edit.action == core::action_id("param.edit") && edit.arg == dir), "instrument arrow " << dir);
    const core::Resolution coarse = resolve_in(instrument, core::GestureKind::Tap, arrow, true);
    CHECK_MESSAGE((coarse.status == core::Resolution::Status::Bound && coarse.action == core::action_id("param.coarse") && coarse.arg == dir), "instrument Shift + arrow " << dir);
    const core::Resolution turn = resolve_in(chooser, core::GestureKind::Tap, arrow, false);
    CHECK_MESSAGE((turn.status == core::Resolution::Status::Bound && turn.action == core::action_id("instrument.turn") && turn.arg == dir), "chooser arrow " << dir);
    const core::Resolution turn_shift = resolve_in(chooser, core::GestureKind::Tap, arrow, true);
    CHECK_MESSAGE((turn_shift.status == core::Resolution::Status::Bound && turn_shift.action == core::action_id("instrument.turn") && turn_shift.arg == dir), "chooser Shift + arrow " << dir);
  }
  // OS-CFG-ARROWS: the same on Config (param.edit / param.coarse) and Record (record.edit / record.coarse); the pages are the F-keys and the screen button, never the arrows.
  const Chain config{"config", {Context::Config, Context::Global}, false};
  const Chain record{"record", {Context::Record, Context::Global}, false};
  for (int dir = -1; dir <= 1; dir += 2) {
    const Button arrow = dir < 0 ? Button::Left : Button::Right;
    for (const bool shift : {false, true}) {
      const core::Resolution c = resolve_in(config, core::GestureKind::Tap, arrow, shift);
      CHECK_MESSAGE((c.status == core::Resolution::Status::Bound && c.action == core::action_id(shift ? "param.coarse" : "param.edit") && c.arg == dir), "config arrow " << dir << " shift " << shift);
      const core::Resolution r = resolve_in(record, core::GestureKind::Tap, arrow, shift);
      CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id(shift ? "record.coarse" : "record.edit") && r.arg == dir), "record arrow " << dir << " shift " << shift);
    }
  }
  // In the Navigator the other F-keys are swallowed, not passed on to the screen under it.
  CHECK(resolve_in(navigator, core::GestureKind::Tap, Button::F4, false).status == core::Resolution::Status::Captured);
  // Shift + Master is the theme, Master alone the Mixer's screen, from every screen; in an overlay neither is reachable.
  for (const Chain* c : {&pattern, &select, &song}) {
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::Master, true).action == core::action_id("ui.theme.next"));
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::Master, false).action == core::action_id("nav.screen"));
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::Pattern, true).action == core::action_id("apps.open"));  // OS-APPSNAV
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::Pattern, false).action == core::action_id("nav.screen"));  // the plain Pattern button is untouched
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::File, true).action == core::action_id("file.save_any"));  // OS-UXF-KEYS 1a
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::File, false).action == core::action_id("nav.screen"));
  }
  const Chain learn{"learn", {Context::Learn, Context::Global}, true};
  CHECK(resolve_in(learn, core::GestureKind::Tap, Button::File, true).status == core::Resolution::Status::Captured);  // OS-UXF-KEYS 1a: an overlay captures it
  CHECK(resolve_in(learn, core::GestureKind::Tap, Button::Master, true).status == core::Resolution::Status::Captured);
  CHECK(resolve_in(navigator, core::GestureKind::Tap, Button::Master, true).status == core::Resolution::Status::Captured);
  // OS-MASTERVOL and OS-SONGPEEK: Master held + the encoder is the master volume, a held Song is the peek and its release the way back; from every screen, and in no capturing overlay.
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const core::Gesture master_turn{0, core::GestureKind::HoldEncoder, app::encoder_ref(0), app::button_ref(Button::Master), 1};
  const core::Gesture song_hold{0, core::GestureKind::Hold, app::button_ref(Button::Song), core::kNoInput, 0};
  const core::Gesture song_end{0, core::GestureKind::HoldEnd, app::button_ref(Button::Song), core::kNoInput, 0};
  for (const Chain* c : {&pattern, &select, &song}) {
    const core::Resolution turn = core::resolve(set, chain_of(*c), master_turn);
    CHECK((turn.status == core::Resolution::Status::Bound && turn.action == core::action_id("mixer.master_vol") && turn.arg == 1));  // (the detents)
    const core::Resolution peek = core::resolve(set, chain_of(*c), song_hold);
    CHECK((peek.status == core::Resolution::Status::Bound && peek.action == core::action_id("nav.peek") && peek.arg == static_cast<core::ActionArg>(app::ScreenId::Song)));
    const core::Resolution back = core::resolve(set, chain_of(*c), song_end);
    CHECK((back.status == core::Resolution::Status::Bound && back.action == core::action_id("nav.peek_end") && back.arg == static_cast<core::ActionArg>(app::ScreenId::Song)));
  }
  CHECK(core::resolve(set, chain_of(learn), master_turn).status == core::Resolution::Status::Captured);
  // OS-PATTEMPO: Pattern held + the encoder is the project tempo from every screen, in no capturing overlay and not in an app's page; F1 held on Pattern + the encoder (OS-PATSWITCH) is not bound here.
  const core::Gesture pattern_turn{0, core::GestureKind::HoldEncoder, app::encoder_ref(0), app::button_ref(Button::Pattern), 1};
  for (const Chain* c : {&pattern, &select, &song}) {
    const core::Resolution turn = core::resolve(set, chain_of(*c), pattern_turn);
    CHECK((turn.status == core::Resolution::Status::Bound && turn.action == core::action_id("song.tempo_hold") && turn.arg == 1));
  }
  CHECK(core::resolve(set, chain_of(learn), pattern_turn).status == core::Resolution::Status::Captured);
  CHECK(core::resolve(set, chain_of(navigator), pattern_turn).status == core::Resolution::Status::Captured);
  CHECK(core::resolve(set, chain_of(Chain{"module app", {Context::ModuleApp, Context::Global}, true}), pattern_turn).status == core::Resolution::Status::Captured);
  const core::Gesture f1_turn{0, core::GestureKind::HoldEncoder, app::encoder_ref(0), app::button_ref(Button::F1), 1};
  CHECK(core::resolve(set, chain_of(song), f1_turn).action != core::action_id("song.tempo_hold"));
  CHECK(core::resolve(set, chain_of(navigator), song_hold).status == core::Resolution::Status::Captured);
  // No Shift binding takes the arrows or the transport and edit keys of the pattern editor: a held Shift still
  // leaves them what they are (select-a-range and the like are left for later, bound where nothing else is). (Shift + F1..F5
  // take the position jumps instead of the "mute track 1-8" keys that the official manual describes, docs/ui/official-manual-map.md, not verified: the maintainer's OS-002b decision, an open question.)
  // (OS-009 P1a.1 bound the arrows, Insert and Delete: they are in the list of Shift rows above; OS-PATCOPY bound Copy (Shift + Copy is a paste); OS-PLAYCUR bound Shift + Play on Pattern: the next block checks it, and that every other screen's Shift + Play is still the plain Play.)
}

TEST_CASE("builtin bindings: in the EVENTS overlay a pad tap is events.pad with the pad's id, and the encoder events.edit (OS-MIDIREC S2b)") {
  const Chain events{"events", {Context::Events, Context::Global}, true};
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const core::Gesture pad_tap{0, core::GestureKind::Tap, app::pad_ref(5), core::kNoInput, 0};
  const core::Resolution pad = core::resolve(set, chain_of(events), pad_tap);
  CHECK_MESSAGE((pad.status == core::Resolution::Status::Bound && pad.action == core::action_id("events.pad") && pad.arg == 5), "pad 5 in events: " << pad.action);
  const core::Gesture enc{0, core::GestureKind::Tap, app::encoder_ref(0), core::kNoInput, 3};
  const core::Resolution turn = core::resolve(set, chain_of(events), enc);
  CHECK_MESSAGE((turn.status == core::Resolution::Status::Bound && turn.action == core::action_id("events.edit") && turn.arg == 3), "the encoder in events: " << turn.action);
}

TEST_CASE("builtin bindings: Shift + Play is play-from-the-cursor on Pattern (OS-PLAYCUR) and the plain Play everywhere else") {
  const Chain pattern{"pattern edit", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain browse{"pattern", {Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  for (const Chain* c : {&pattern, &browse}) {
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::Play, true).action == core::action_id("transport.play_cursor"));
    CHECK(resolve_in(*c, core::GestureKind::Tap, Button::Play, false).action == core::action_id("transport.play"));
  }
  const core::Resolution on_song = resolve_in(song, core::GestureKind::Tap, Button::Play, true);
  CHECK(on_song.action == resolve_in(song, core::GestureKind::Tap, Button::Play, false).action);
}

TEST_CASE("builtin bindings: Shift + Up / Down keep their own meaning where they have one, and are a page only on the lists (OS-NAV3)") {
  const auto resolves = [](const Chain& c, bool up, const char* action, int arg) {
    const core::Resolution r = resolve_in(c, core::GestureKind::Tap, up ? Button::Up : Button::Down, true);
    CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id(action) && r.arg == arg), c.what << (up ? " Shift+Up " : " Shift+Down ") << action);
  };
  // The contexts that keep their Shift + arrows: Pattern the block, Song the range, Perform (Shift + Left / Right swap; Shift + Up / Down fall through to the plain FX keys), Chop the even count, Sample edit the amplitude, an app its row.
  const Chain pattern{"pattern edit", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain perform{"perform", {Context::Perform, Context::Global}, false};
  const Chain chop{"chop", {Context::WaveChop, Context::Wave, Context::Global}, false};
  const Chain sample_edit{"sample edit", {Context::SampleEdit, Context::Global}, false};
  const Chain app_page{"module app", {Context::ModuleApp, Context::Global}, true};
  resolves(pattern, true, "select.extend", -1);
  resolves(pattern, false, "select.extend", 1);
  resolves(song, true, "song.select", -1);
  resolves(song, false, "song.select", 1);
  resolves(perform, true, "perform.fx", 1);  // (the plain Up / Down: the slot's FX, which a held Shift falls through to)
  resolves(perform, false, "perform.fx", -1);
  resolves(chop, true, "wave.even_n", 1);
  resolves(chop, false, "wave.even_n", -1);
  resolves(sample_edit, true, "sedit.amp", 1);
  resolves(sample_edit, false, "sedit.amp", -1);
  resolves(app_page, true, "nav.up", 0);
  resolves(app_page, false, "nav.down", 0);
  // The lists: a page, one for each context that has a list to move.
  for (const Context ctx : {Context::File, Context::SampleLoad, Context::Modules}) {
    resolves({"list", {ctx, Context::Global}, false}, true, "cursor.page", -1);
    resolves({"list", {ctx, Context::Global}, false}, false, "cursor.page", 1);
  }
  for (const Context ctx : {Context::ModulePicker, Context::Preset, Context::Learn}) {
    resolves({"overlay list", {ctx, Context::Global}, true}, true, "cursor.page", -1);
    resolves({"overlay list", {ctx, Context::Global}, true}, false, "cursor.page", 1);
  }
  resolves({"step popup", {Context::StepPopup, Context::Global}, true}, true, "popup.page", -1);
  resolves({"step popup", {Context::StepPopup, Context::Global}, true}, false, "popup.page", 1);
  // And the plain arrows of those lists are what they were.
  for (const Context ctx : {Context::File, Context::SampleLoad, Context::Modules}) {
    const core::Resolution plain = resolve_in({"list", {ctx, Context::Global}, false}, core::GestureKind::Tap, Button::Down, false);
    CHECK((plain.status == core::Resolution::Status::Bound && plain.action == core::action_id("nav.down")));
  }
}

TEST_CASE("builtin bindings: on an app page F8 is BACK and Play the transport's, F7 + F8 still leaves, Shift + F8 reads nothing, and F1..F7 are the app's (OS-KEYROWDLG S5)") {
  const Chain app_page{"module app", {Context::ModuleApp, Context::Global}, true};
  const auto bound = [&](Button b, bool shift, const char* action) {
    const core::Resolution r = resolve_in(app_page, core::GestureKind::Tap, b, shift);
    CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id(action)), "button " << static_cast<int>(b) << (shift ? " with Shift" : "") << " should be " << action);
  };
  bound(Button::Play, false, "transport.play");  // the page captures, so the global Play is bound here: the transport runs and the app never sees Play
  bound(Button::F8, false, "module.exit");       // BACK, on every app page; the app never sees F8
  bound(Button::F8, true, "ui.none");            // Shift + F8 reads nothing and must not fall through to BACK
  // F1..F7, plain and with Shift, are not the OS's on this page: the chain captures them for the app
  for (const Button k : {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7}) {
    for (const bool shift : {false, true}) {
      CHECK_MESSAGE(resolve_in(app_page, core::GestureKind::Tap, k, shift).status == core::Resolution::Status::Captured, "F" << static_cast<int>(k) - static_cast<int>(Button::F1) + 1 << (shift ? " with Shift" : ""));
    }
  }
  // The chord is still the escape hatch: bound in the page's context to the same exit
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const core::ContextChain chain = chain_of(app_page);
  CHECK(core::chord_bound(set, chain, app::button_ref(Button::F7), app::button_ref(Button::F8)));
  const core::Trigger t = core::chord(app::button_ref(Button::F7), app::button_ref(Button::F8));
  const core::Resolution r = core::resolve(set, chain, core::Gesture{0, t.gesture, t.input, t.modifier, 0});
  CHECK((r.status == core::Resolution::Status::Bound && r.action == core::action_id("module.exit")));
}

TEST_CASE("builtin bindings: every screen button has its nav.screen binding, and the Navigator's open and close are bound") {
  const core::BindingView builtin = app::builtin_bindings();
  const app::ScreenTable table = app::screen_registry();
  for (std::size_t i = 0; i < table.count; ++i) {
    const app::ScreenEntry& e = table.items[i];
    if (e.hidden || e.buttonless) {
      continue;  // OS-007d: a diagnostic with no screen button: the platform opens it from a boot chord. OS-012b: MODULES is in the Navigator and has no button
    }
    int found = 0;
    for (std::size_t k = 0; k < builtin.count; ++k) {
      const core::Binding& b = builtin.items[k];
      if (b.action == core::action_id("nav.screen") && b.trigger == core::tap(app::button_ref(e.button)) && b.context != app::context_id(Context::ModuleApp)) {
        ++found;
        CHECK(b.context == app::context_id(Context::Global));
        CHECK(b.arg_from == core::ArgFrom::Fixed);
        CHECK(b.arg == static_cast<core::ActionArg>(e.id));
      }
    }
    CHECK_MESSAGE(found == 1, e.name << " has " << found << " nav.screen bindings");
  }
  // And nothing else is bound to nav.screen: the Navigator opens with the chords Master + Song and Song + Pattern (OS-APPSNAV, before it OS-NAV2's Shift + Pattern), so Pattern has no Hold
  // binding to wait for any more (a button with none taps at the press) and no long-press twin of the Pattern button is needed.
  std::size_t total = 0, app_total = 0;
  for (std::size_t k = 0; k < builtin.count; ++k) {
    const core::Binding& b = builtin.items[k];
    total += b.action == core::action_id("nav.screen") && b.context != app::context_id(Context::ModuleApp) ? 1u : 0u;
    app_total += b.action == core::action_id("nav.screen") && b.context == app::context_id(Context::ModuleApp) ? 1u : 0u;
    CHECK_FALSE((b.action == core::action_id("nav.screen") && b.trigger.gesture == core::GestureKind::Hold));
    CHECK_FALSE((b.trigger.input == app::button_ref(Button::Pattern) && (b.trigger.gesture == core::GestureKind::Hold || b.trigger.gesture == core::GestureKind::HoldEnd)));
  }
  std::size_t with_button = 0;  // rows with a screen button of their own: not the hidden ones (OS-007d, OS-012b) and not MODULES, which only the Navigator reaches
  for (std::size_t i = 0; i < table.count; ++i) {
    with_button += table.items[i].hidden || table.items[i].buttonless ? 0u : 1u;
  }
  CHECK(total == with_button + 2u);  // the buttons, the INPUT TEST screen's chord out of it, and F6 BACK on an insert's page (OS-INSKEYS)
  CHECK(app_total == with_button);   // OS-APPUX S3: on an app's page (the ModuleApp context) every screen button leaves the app for its screen, the same ones, one each
}

TEST_CASE("builtin bindings: Master + Song and Song + Pattern open the Navigator from every screen, in either order, and the apps list closes with Pattern (OS-APPSNAV)") {
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const Chain pattern{"pattern", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain config{"config", {Context::Config, Context::Global}, false};
  const Chain navigator{"navigator", {Context::Navigator, Context::Global}, true};
  const Chain picker{"picker", {Context::ModulePicker, Context::Global}, true};
  const Button pairs[2][2] = {{Button::Master, Button::Song}, {Button::Song, Button::Pattern}};
  for (const Chain* c : {&pattern, &song, &config}) {
    for (const auto& pair : pairs) {
      CHECK(core::chord_bound(set, chain_of(*c), app::button_ref(pair[0]), app::button_ref(pair[1])));
      CHECK(core::chord_bound(set, chain_of(*c), app::button_ref(pair[1]), app::button_ref(pair[0])));
      for (int swap = 0; swap < 2; ++swap) {
        const core::Trigger t = core::chord(app::button_ref(pair[swap]), app::button_ref(pair[1 - swap]));
        const core::Resolution r = core::resolve(set, chain_of(*c), core::Gesture{0, t.gesture, t.input, t.modifier, 0});
        CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id("navigator.open")), c->what << " chord " << swap);
      }
    }
  }
  CHECK_FALSE(core::chord_bound(set, chain_of(pattern), app::button_ref(Button::Master), app::button_ref(Button::Pattern)));  // the third pair is not one
  CHECK_FALSE(core::chord_bound(set, chain_of(navigator), app::button_ref(Button::Master), app::button_ref(Button::Song)));   // a capturing context hides Global's chords
  // The list of the apps: Pattern and Shift + Pattern (which falls back to the plain tap) close it, F8 is BACK (it launched before OS-UXF-KEYS item 5), F1 launches.
  for (const bool shift : {false, true}) {
    const core::Resolution r = resolve_in(picker, core::GestureKind::Tap, Button::Pattern, shift);
    CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id("apps.close")), "picker Pattern shift " << shift);
  }
  CHECK(resolve_in(picker, core::GestureKind::Tap, Button::F8, false).action == core::action_id("ui.back"));
  CHECK(resolve_in(picker, core::GestureKind::Tap, Button::F1, false).action == core::action_id("module.pick"));
}

TEST_CASE("builtin bindings: Master + Insert opens the Mixer INSERTS page from every screen, in either order; the plain Insert keeps its own actions (OS-MIXINSKEY)") {
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const Chain pattern{"pattern", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain instrument{"instrument", {Context::Instrument, Context::Global}, false};
  const Chain stems{"stems", {Context::MixerStems, Context::Mixer, Context::Global}, false};
  const Chain inserts{"inserts", {Context::MixerInserts, Context::Mixer, Context::Global}, false};
  const Chain navigator{"navigator", {Context::Navigator, Context::Global}, true};
  for (const Chain* c : {&pattern, &song, &instrument, &stems, &inserts}) {
    CHECK_MESSAGE(core::chord_bound(set, chain_of(*c), app::button_ref(Button::Master), app::button_ref(Button::Insert)), c->what);
    CHECK_MESSAGE(core::chord_bound(set, chain_of(*c), app::button_ref(Button::Insert), app::button_ref(Button::Master)), c->what);
    for (int swap = 0; swap < 2; ++swap) {
      const core::Trigger t = swap == 0 ? core::chord(app::button_ref(Button::Master), app::button_ref(Button::Insert)) : core::chord(app::button_ref(Button::Insert), app::button_ref(Button::Master));
      const core::Resolution r = core::resolve(set, chain_of(*c), core::Gesture{0, t.gesture, t.input, t.modifier, 0});
      CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id("mixer.inserts")), c->what << " chord " << swap);
    }
  }
  CHECK_FALSE(core::chord_bound(set, chain_of(navigator), app::button_ref(Button::Master), app::button_ref(Button::Insert)));  // a capturing context hides Global's chord
  // The plain Insert is unchanged where it was bound.
  CHECK(resolve_in(pattern, core::GestureKind::Tap, Button::Insert, false).action == core::action_id("step.insert"));
  CHECK(resolve_in(song, core::GestureKind::Tap, Button::Insert, false).action == core::action_id("song.add_slot"));
  CHECK(resolve_in(stems, core::GestureKind::Tap, Button::Insert, false).action == core::action_id("mixer.stem_set"));
  CHECK(resolve_in(pattern, core::GestureKind::Tap, Button::Master, false).action == core::action_id("nav.screen"));
}

TEST_CASE("builtin bindings: Master held, then a tap of Insert, opens the Mixer INSERTS page from every screen; Master is a held modifier there and nothing else changes (OS-MIXINSHOLD)") {
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const Chain pattern{"pattern", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain instrument{"instrument", {Context::Instrument, Context::Global}, false};
  const Chain stems{"stems", {Context::MixerStems, Context::Mixer, Context::Global}, false};
  const Chain inserts{"inserts", {Context::MixerInserts, Context::Mixer, Context::Global}, false};
  const Chain navigator{"navigator", {Context::Navigator, Context::Global}, true};
  const core::Gesture held{0, core::GestureKind::Tap, app::button_ref(Button::Insert), app::button_ref(Button::Master), 0};
  for (const Chain* c : {&pattern, &song, &instrument, &stems, &inserts}) {
    const core::Resolution r = core::resolve(set, chain_of(*c), held);  // a screen's plain Insert row does not shadow the Global one
    CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id("mixer.inserts")), c->what);
    CHECK_MESSAGE((core::gesture_wants(set, chain_of(*c), app::button_ref(Button::Master)) & core::kWantModifier) != 0, c->what);  // Master is a held modifier in every chain that binds the row
    CHECK_MESSAGE((core::gesture_wants(set, chain_of(*c), app::button_ref(Button::Insert)) & core::kWantModifier) == 0, c->what);  // Insert is not
  }
  // A capturing context hides the Global row: Master + Insert there is the plain Insert of the context (or nothing).
  CHECK_FALSE(core::resolve(set, chain_of(navigator), held).action == core::action_id("mixer.inserts"));
  // The other Master keys are as they were: Master + Shift + Insert is Shift's row, Master's tap, Shift + Master, the chord with Song and the encoder hold.
  CHECK(resolve_in(pattern, core::GestureKind::Tap, Button::Insert, true).action == core::action_id("cursor.jump_fraction"));
  CHECK(resolve_in(pattern, core::GestureKind::Tap, Button::Master, false).action == core::action_id("nav.screen"));
  CHECK(resolve_in(pattern, core::GestureKind::Tap, Button::Master, true).action == core::action_id("ui.theme.next"));
  CHECK(core::chord_bound(set, chain_of(pattern), app::button_ref(Button::Master), app::button_ref(Button::Song)));
  CHECK(core::hold_encoder_bound(set, chain_of(pattern), app::button_ref(Button::Master), core::InputRef{core::InputKind::Encoder, 0}));
}

TEST_CASE("builtin bindings: F8 is UNDO and Shift + F8 REDO on Pattern (every mode) and Song, and Back in the overlays (OS-002c)") {
  const Chain edit{"pattern edit", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain select{"pattern select", {Context::PatternSelect, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain instrument{"instrument", {Context::Instrument, Context::Global}, false};  // OS-002d
  for (const Chain* c : {&edit, &select, &song, &instrument}) {
    const core::Resolution undo = resolve_in(*c, core::GestureKind::Tap, Button::F8, false);
    CHECK_MESSAGE((undo.status == core::Resolution::Status::Bound && undo.action == core::action_id("edit.undo")), c->what);
    const core::Resolution redo = resolve_in(*c, core::GestureKind::Tap, Button::F8, true);
    CHECK_MESSAGE((redo.status == core::Resolution::Status::Bound && redo.action == core::action_id("edit.redo")), c->what);
    // The other F-keys are still the soft keys of the screen.
    const core::Resolution f7 = resolve_in(*c, core::GestureKind::Tap, Button::F7, false);
    CHECK_MESSAGE((f7.status == core::Resolution::Status::Bound && f7.action == core::action_id(c == &song ? "song.tempo" : (c == &instrument ? "instrument.step" : (c == &edit || c == &select ? "verb.open" : "ui.softkey")))),
                  c->what);  // (OS-INSTUI: F7 is INS> on the Instrument screen; OS-H3A: TOOLS on Pattern)  // OS-SONG1: F7 is TEMPO on the song (OS-PADS-A: EXPORT moved to Shift + Fx)
  }
  // The overlays capture and bind F8 as Back; a held Shift falls through to it (they have no Shift + F8 of their own). OS-UXF-KEYS item 5: the Navigator follows the same grammar, F1 is GO, F8 is BACK (F7 is nothing; Pattern also closes it).
  const Chain learn{"learn", {Context::Learn, Context::Global}, true};
  const Chain navigator{"navigator", {Context::Navigator, Context::Global}, true};
  for (const bool shift : {false, true}) {
    const core::Resolution back = resolve_in(learn, core::GestureKind::Tap, Button::F8, shift);
    CHECK_MESSAGE((back.status == core::Resolution::Status::Bound && back.action == core::action_id("ui.back")), "learn");
    const core::Resolution go = resolve_in(navigator, core::GestureKind::Tap, Button::F1, shift);
    CHECK_MESSAGE((go.status == core::Resolution::Status::Bound && go.action == core::action_id("navigator.accept")), "navigator F1 is GO");
    const core::Resolution f7 = resolve_in(navigator, core::GestureKind::Tap, Button::F7, shift);
    CHECK_MESSAGE((f7.status == core::Resolution::Status::Bound && f7.action == core::action_id("ui.none")), "navigator F7 is nothing");
    for (const Button close : {Button::F8, Button::Pattern}) {
      const core::Resolution r = resolve_in(navigator, core::GestureKind::Tap, close, shift);
      CHECK_MESSAGE((r.status == core::Resolution::Status::Bound && r.action == core::action_id("ui.back")), "navigator BACK " << static_cast<int>(close));
    }
  }
  // Nothing in the Navigator answers a release: no HoldEnd of any button, and the end of a hold of Pattern is not an accept.
  for (const Button b : {Button::Pattern, Button::Shift, Button::F1, Button::F7, Button::F8}) {
    const core::Resolution r = resolve_in(navigator, core::GestureKind::HoldEnd, b, false);
    CHECK_MESSAGE(r.status == core::Resolution::Status::Captured, "holdend " << static_cast<int>(b));
  }
  // The history actions are registered, and edit.undo / edit.redo are not themselves undoable.
  for (const char* name : {"edit.undo", "edit.redo", "macro.record", "macro.play", "job.cancel", "project.tempo"}) {
    CHECK_MESSAGE(app::find_action(app::action_registry(), core::action_id(name)) != nullptr, name);
  }
}

// OS-LIVEREC: the Select mode (and, since OS-004d, Follow) is retired; its context stays named for the keymaps that mention it, with nothing bound, and the block is Shift + the arrows
// of the Pattern context, in every mode. No built-in binding cycles the modes.
TEST_CASE("builtin bindings: the retired Follow and Select contexts bind nothing; no built-in key is the mode cycle; the block is Shift + the arrows in every mode") {
  const core::BindingView builtin = app::builtin_bindings();
  for (std::size_t i = 0; i < builtin.count; ++i) {
    const core::Binding& b = builtin.items[i];
    CHECK(b.context != app::context_id(Context::PatternSelect));
    CHECK(b.context != app::context_id(Context::PatternFollow));
    CHECK(b.action != core::action_id("view.mode_next"));
  }
  for (const Context mode : {Context::PatternEdit, Context::PatternBrowse}) {
    const Chain chain{"pattern mode", {mode, Context::Pattern, Context::Global}, false};
    const struct {
      Button button;
      int arg;
    } arrows[] = {{Button::Left, -10}, {Button::Right, 10}, {Button::Up, -1}, {Button::Down, 1}};
    for (const auto& a : arrows) {
      const core::Resolution r = resolve_in(chain, core::GestureKind::Tap, a.button, true);
      CHECK((r.status == core::Resolution::Status::Bound && r.action == core::action_id("select.extend") && r.arg == a.arg));
    }
  }
  // Shift + Rec is Edit following / free wherever the screen has no key of its own for it, and Rec is the Edit toggle (free) on Pattern.
  const Chain pattern{"pattern", {Context::PatternBrowse, Context::Pattern, Context::Global}, false};
  const core::Resolution free_rec = resolve_in(pattern, core::GestureKind::Tap, Button::Rec, true);
  CHECK((free_rec.status == core::Resolution::Status::Bound && free_rec.action == core::action_id("pattern.rec_free")));
  const core::Resolution rec = resolve_in(pattern, core::GestureKind::Tap, Button::Rec, false);
  CHECK((rec.status == core::Resolution::Status::Bound && rec.action == core::action_id("pattern.edit_toggle")));
}

// OS-DELREPEAT: a held button's repeats are Taps flagged `repeat`, and dispatch() lets only a kActionRepeatable action take one (the press itself always runs). So an action bound as a
// Tap to a button of kRepeatButtons must carry the flag, or it would lose the repeat it had when every Tap repeated; and of the Delete bindings only the pattern's step.clear and
// step.pull repeat (a held Delete must not repeat file.delete, song.delete_slot, ...). Every screen is covered: the bindings of every context are walked.
TEST_CASE("builtin bindings: every Tap on a repeat button is a repeatable action; of Delete's only step.clear and step.pull are") {
  const core::BindingView builtin = app::builtin_bindings();
  const auto repeatable = [](core::ActionId id) {
    const app::ActionDef* a = app::find_action(app::action_registry(), id);
    return a != nullptr && (a->flags & core::kActionRepeatable) != 0;
  };
  const auto name_of = [](core::ActionId id) {
    const app::ActionDef* a = app::find_action(app::action_registry(), id);
    return std::string(a != nullptr ? a->name : "?");
  };
  std::size_t arrows = 0;
  std::size_t deletes = 0;
  for (std::size_t i = 0; i < builtin.count; ++i) {
    const core::Binding& b = builtin.items[i];
    if (b.trigger.gesture != core::GestureKind::Tap || b.trigger.input.kind != core::InputKind::Button) {
      continue;
    }
    const std::uint8_t id = b.trigger.input.id;
    if (id < 64 && ((app::kRepeatButtons >> id) & 1u) != 0 && id != static_cast<std::uint8_t>(Button::Delete)) {
      ++arrows;
      CHECK_MESSAGE(repeatable(b.action), "button " << unsigned{id} << " in context " << unsigned{b.context} << " binds " << name_of(b.action) << ", which is not kActionRepeatable");
    }
    if (id == static_cast<std::uint8_t>(Button::Delete)) {
      ++deletes;
      const std::string n = name_of(b.action);
      CHECK_MESSAGE(repeatable(b.action) == (n == "step.clear" || n == "step.pull"), "Delete in context " << unsigned{b.context} << " binds " << n << ": the repeat flag is wrong");
    }
  }
  CHECK(((app::kRepeatButtons >> static_cast<unsigned>(Button::Delete)) & 1u) != 0u);
  CHECK(arrows >= 40u);  // the walk is not vacuous
  CHECK(deletes >= 10u);
}

// ---- OS-SONGSHARE S3: Copy is a second bound modifier, on Song only: every built-in binding that names it is one of these ----

TEST_CASE("builtin bindings: the Copy rows (OS-SONGSHARE S3) are Song's, make Copy a held modifier only there, and the Song rows of Copy and Shift + Copy are unchanged") {
  const core::BindingView builtin = app::builtin_bindings();
  std::vector<std::string> copied;
  for (std::size_t i = 0; i < builtin.count; ++i) {
    const core::Binding& b = builtin.items[i];
    if (b.trigger.modifier == app::button_ref(Button::Copy)) {
      copied.push_back(binding_line(b));
    }
  }
  std::vector<std::string> expected = {"song tap copy+up song.move -1\n", "song tap copy+down song.move 1\n", "song tap copy+left song.move_along -1\n", "song tap copy+right song.move_along 1\n",  // (OS-SONG3 S2: the same move along TIMELINE)
      "song tap copy+f3 song.add_empty 0\n"};  // OS-SONG3 S5: Copy held + F3, ADD EMPTY
  std::sort(copied.begin(), copied.end());
  std::sort(expected.begin(), expected.end());
  CHECK(copied == expected);

  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, builtin}};
  const auto wants = [&](const Chain& c, Button b) { return core::gesture_wants(set, chain_of(c), app::button_ref(b)); };
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain song_ask{"song question", {Context::SongAsk, Context::Song, Context::Global}, false};
  const Chain pattern{"pattern", {Context::PatternNote, Context::Pattern, Context::Global}, false};
  const Chain perform{"perform", {Context::Perform, Context::Global}, false};
  CHECK(wants(song, Button::Copy) == core::kWantModifier);  // its own Tap waits for the release, nothing else
  CHECK(wants(song_ask, Button::Copy) == core::kWantModifier);
  CHECK(wants(pattern, Button::Copy) == 0);  // Pattern and Perform keep Copy at the press
  CHECK(wants(perform, Button::Copy) == 0);
  for (const Button b : {Button::Up, Button::Down, Button::Left, Button::Right, Button::Delete, Button::Note}) {
    CHECK((wants(song, b) & core::kWantModifier) == 0);
  }
  const auto resolved = [&](const Chain& c, core::GestureKind kind, Button button, Button mod) {
    return core::resolve(set, chain_of(c), core::Gesture{0, kind, app::button_ref(button), app::button_ref(mod), 0});
  };
  CHECK(resolved(song, core::GestureKind::Tap, Button::Up, Button::Copy).action == core::action_id("song.move"));
  CHECK(resolved(song, core::GestureKind::Tap, Button::Down, Button::Copy).arg == 1);
  CHECK(resolved(song, core::GestureKind::Tap, Button::Up, Button::Copy).arg == -1);
  CHECK(resolved(song, core::GestureKind::Tap, Button::Left, Button::Copy).action == core::action_id("song.move_along"));  // OS-SONG3 S2: Copy + Left / Right is the move along TIMELINE
  CHECK(resolved(song, core::GestureKind::Tap, Button::Right, Button::Copy).arg == 1);
  CHECK(resolved(song, core::GestureKind::Tap, Button::Left, Button::Copy).arg == -1);
  CHECK(resolved(song, core::GestureKind::Tap, Button::F3, Button::Copy).action == core::action_id("song.add_empty"));  // OS-SONG3 S5: ADD EMPTY; the plain F3 is still ADD SLOT
  CHECK(core::resolve(set, chain_of(song), core::Gesture{0, core::GestureKind::Tap, app::button_ref(Button::F3), core::kNoInput, 0}).action == core::action_id("song.add_slot"));
  CHECK(resolved(song, core::GestureKind::Tap, Button::Up, Button::Shift).action == core::action_id("song.select"));  // Shift wins
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Up, Button::Copy).action != core::action_id("song.move"));  // not on Pattern
  CHECK(resolved(song, core::GestureKind::Tap, Button::Copy, Button::Shift).action == core::action_id("song.paste"));
  CHECK(resolved(song, core::GestureKind::Tap, Button::Copy, Button::Copy).action == core::action_id("song.copy"));

  // PAT-OPS-B: in the choosers (F1 held on Pattern is the PATTERN chooser) Copy and Shift + Copy are the whole-pattern clip, and Copy is no modifier there; Pattern's own rows stay block.copy / block.paste.
  const Chain chooser{"chooser", {Context::Chooser, Context::Global}, false};
  CHECK(wants(chooser, Button::Copy) == 0);
  CHECK(resolved(chooser, core::GestureKind::Tap, Button::Copy, Button::Copy).action == core::action_id("pattern.clip_copy"));
  CHECK(resolved(chooser, core::GestureKind::Tap, Button::Copy, Button::Shift).action == core::action_id("pattern.clip_paste"));
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Copy, Button::Copy).action == core::action_id("block.copy"));
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Copy, Button::Shift).action == core::action_id("block.paste"));
}

// ---- OS-ZOOM: Note is the one bound modifier (core/gesture.h, kWantModifier): every built-in binding that names it is one of these ----

TEST_CASE("builtin bindings: the Note rows (OS-ZOOM) are Pattern's and the step popup's, and make Note a held modifier only there") {
  const core::BindingView builtin = app::builtin_bindings();
  std::vector<std::string> noted;
  for (std::size_t i = 0; i < builtin.count; ++i) {
    const core::Binding& b = builtin.items[i];
    if (b.trigger.modifier == app::button_ref(Button::Note)) {
      noted.push_back(binding_line(b));
    }
  }
  std::vector<std::string> expected;
  for (const char* context : {"pattern", "steppopup"}) {
    expected.push_back(std::string(context) + " tap note+instr pattern.zoom 0\n");
    expected.push_back(std::string(context) + " tap note+left cursor.track -1\n");
    expected.push_back(std::string(context) + " tap note+right cursor.track 1\n");
  }
  std::sort(noted.begin(), noted.end());
  std::sort(expected.begin(), expected.end());
  CHECK(noted == expected);

  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, builtin}};
  const auto wants = [&](const Chain& c, Button b) { return core::gesture_wants(set, chain_of(c), app::button_ref(b)); };
  const Chain pattern{"pattern", {Context::PatternNote, Context::Pattern, Context::Global}, false};
  const Chain song{"song", {Context::Song, Context::Global}, false};
  const Chain popup{"popup", {Context::StepPopup, Context::Pattern, Context::Global}, true};
  CHECK(wants(pattern, Button::Note) == (core::kWantHold | core::kWantModifier));  // Hold (the Note popup) and the modifier
  CHECK(wants(popup, Button::Note) == (core::kWantHold | core::kWantModifier));  // (HoldEnd, the popup's release, waits for the hold as a Hold does)
  CHECK(wants(song, Button::Note) == 0);  // nowhere else is Note held back
  CHECK((wants(pattern, Button::Shift) & core::kWantModifier) != 0);  // (the recogniser ignores the bit for Shift)
  for (const Button b : {Button::Instr, Button::Vol, Button::Fx, Button::Left, Button::Right}) {
    CHECK((wants(pattern, b) & core::kWantModifier) == 0);
  }
  // Resolved: the Note rows win under Note, and a button with no Note row falls back to its plain row (Note + Vol is Vol).
  const auto resolved = [&](const Chain& c, core::GestureKind kind, Button button, Button mod) {
    return core::resolve(set, chain_of(c), core::Gesture{0, kind, app::button_ref(button), app::button_ref(mod), 0});
  };
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Instr, Button::Note).action == core::action_id("pattern.zoom"));
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Right, Button::Note).action == core::action_id("cursor.track"));
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Vol, Button::Note).action == core::action_id("pattern.column"));
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Fx, Button::Note).action == core::action_id("pattern.fx_column"));
  CHECK(resolved(popup, core::GestureKind::Tap, Button::Instr, Button::Note).action == core::action_id("pattern.zoom"));
  CHECK(resolved(popup, core::GestureKind::Tap, Button::Up, Button::Note).action == core::action_id("popup.move"));  // the popup's own rows stay under Note
  CHECK(resolved(pattern, core::GestureKind::Tap, Button::Instr, Button::Shift).action == core::action_id("select.jump"));  // OS-SELSHIFT
}

TEST_CASE("builtin bindings: F7 in the Learn overlay assigns the encoder target; the chord Instr + FX2 is gone (OS-ENCLEARN, OS-UXF-KEYS item 17)") {
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const Chain pattern{"pattern", {Context::PatternNote, Context::Pattern, Context::Global}, false};
  const Chain instrument{"instrument", {Context::Instrument, Context::Global}, false};
  const Chain learn{"learn", {Context::Learn, Context::Global}, true};
  const core::Gesture assign{0, core::GestureKind::Chord, app::button_ref(Button::Instr), app::button_ref(Button::Fx), 0};
  const core::Gesture assign_swapped{0, core::GestureKind::Chord, app::button_ref(Button::Fx), app::button_ref(Button::Instr), 0};
  const auto assigns = [&](const Chain& c) {  // (the recogniser hands the pair over in canonical order; either spelling resolves)
    for (const core::Gesture& g : {assign, assign_swapped}) {
      const core::Resolution r = core::resolve(set, chain_of(c), g);
      if (r.status == core::Resolution::Status::Bound && r.action == core::action_id("enc.assign")) {
        return true;
      }
    }
    return false;
  };
  CHECK_FALSE(assigns(pattern));
  CHECK_FALSE(assigns(instrument));
  CHECK_FALSE(assigns(learn));
  CHECK_FALSE(core::chord_bound(set, chain_of(pattern), app::button_ref(Button::Fx), app::button_ref(Button::Instr)));
  const core::Resolution f7 = resolve_in(learn, core::GestureKind::Tap, Button::F7, false);
  CHECK(f7.status == core::Resolution::Status::Bound);
  CHECK(f7.action == core::action_id("enc.assign"));
  CHECK(resolve_in(pattern, core::GestureKind::Tap, Button::F7, false).action != core::action_id("enc.assign"));
  // the turn itself is not a binding: App::run_once takes it ahead of the gesture layer
  CHECK(core::resolve(set, chain_of(pattern), core::Gesture{0, core::GestureKind::HoldEncoder, app::encoder_ref(0), app::button_ref(Button::Instr), 1}).action != core::action_id("enc.turn"));
}

TEST_CASE("builtin bindings: in the EVENTS overlay Insert, Delete, F2 and F7 are the list's own actions (OS-MIDIREC S2c, S3)") {
  const Chain events{"events", {Context::Events, Context::Global}, true};
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const auto bound = [&](Button b, bool shift, const char* action, int arg) {
    const core::Resolution r = core::resolve(set, chain_of(events), core::Gesture{0, core::GestureKind::Tap, app::button_ref(b), shift ? app::button_ref(Button::Shift) : core::kNoInput, 0});
    return r.status == core::Resolution::Status::Bound && r.action == core::action_id(action) && r.arg == arg;
  };
  CHECK(bound(Button::Insert, false, "events.insert", 0));
  CHECK(bound(Button::Insert, true, "events.insert", 1));
  CHECK(bound(Button::Delete, false, "events.delete", 0));
  CHECK(bound(Button::F2, false, "events.quant", 0));
  CHECK(bound(Button::F2, true, "events.quant", 1));
  CHECK(bound(Button::F7, false, "events.undo", 0));
  CHECK(bound(Button::F7, true, "events.redo", 0));
}

TEST_CASE("builtin bindings: OS-PTOOLS2 S3b, F7 held + pad runs the verb without the menu (verb.hold_pad), a tap of F7 still opens it, and the hold itself is verb.hold") {
  const core::BindingSet set{{core::BindingView{nullptr, 0}, core::BindingView{nullptr, 0}, app::builtin_bindings()}};
  const Chain edit{"pattern edit", {Context::PatternEdit, Context::Pattern, Context::Global}, false};
  const Chain note{"pattern note", {Context::PatternNote, Context::Pattern, Context::Global}, false};  // a field level: its plain pads do not answer under F7
  const auto res = [&](const Chain& c, core::GestureKind kind, core::InputRef input, core::InputRef mod) {
    return core::resolve(set, chain_of(c), core::Gesture{0, kind, input, mod, 0});
  };
  for (const Chain* c : {&edit, &note}) {
    const core::Resolution pad = res(*c, core::GestureKind::Tap, app::pad_ref(6), app::button_ref(Button::F7));
    CHECK_MESSAGE((pad.status == core::Resolution::Status::Bound && pad.action == core::action_id("verb.hold_pad") && pad.arg == 6), c->what);
    const core::Resolution any = res(*c, core::GestureKind::Tap, app::pad_ref(core::kAnyId), app::button_ref(Button::F7));
    CHECK_MESSAGE((any.status == core::Resolution::Status::Bound && any.action == core::action_id("verb.hold_pad")), c->what);
    // without the held F7 the pad is the field's / mode's own (a plain row), never the verb
    const core::Resolution plain = res(*c, core::GestureKind::Tap, app::pad_ref(6), core::kNoInput);
    CHECK_MESSAGE(plain.action != core::action_id("verb.hold_pad"), c->what);
  }
  const core::Resolution tap = res(edit, core::GestureKind::Tap, app::button_ref(Button::F7), core::kNoInput);
  CHECK_MESSAGE((tap.status == core::Resolution::Status::Bound && tap.action == core::action_id("verb.open")), "a tap of F7 still opens the menu");
  const core::Resolution hold = res(edit, core::GestureKind::Hold, app::button_ref(Button::F7), core::kNoInput);
  CHECK_MESSAGE((hold.status == core::Resolution::Status::Bound && hold.action == core::action_id("verb.hold")), "the hold of F7");
  // F7 is a held modifier only where a binding names it: the wants of the chain say so, and the Song screen does not have the pad rows
  const core::Resolution song_pad = res(Chain{"song", {Context::Song, Context::Global}, false}, core::GestureKind::Tap, app::pad_ref(6), app::button_ref(Button::F7));
  CHECK_MESSAGE(song_pad.action != core::action_id("verb.hold_pad"), "no verb.hold_pad on Song");
}
