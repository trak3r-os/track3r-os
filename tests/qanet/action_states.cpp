// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A1: the tables of action_states.h. Read that header first. Entry order is the registry's alphabet, so a new action finds its place; `A(name)` is "the demo document, nothing to set up" (the
// action acts on what the document and the screen already hold), `S(label, doc, steps)` a state that needs something more. An action may have several states (a used slot and a module slot, with and
// without a selection): it is run once in each.
#include "action_states.h"

#include <utility>

namespace qanet {

namespace {

using o3os::app::Context;

[[maybe_unused]] State S(const char* name, Doc doc, const char* steps, const char* pre = "") { return State{name, doc, steps, pre}; }
ActionEntry A(const char* action) { return ActionEntry{action, {State{"demo", Doc::Demo, ""}}}; }
[[maybe_unused]] ActionEntry A(const char* action, std::vector<State> states) { return ActionEntry{action, std::move(states)}; }


// ---- States, by what they put in front of the action ------------------------------------------------------------------------------------------------------------------------------------------------
// Pattern editing: Rec turns editing on (Browse only previews), the cursor is on the first step of the first track of the demo pattern; a selection is Shift + Down; a clip is Copy, then a move down.
const State kEdit = {"edit on", Doc::Demo, "tap rec\n"};
// A step that already has a volume and an effect (the deltas of those columns have something to move).
const State kEditFilled = {"edit on, step with volume and effect", Doc::Demo, "tap rec\naction step.vol_pad 5\naction step.fx_pad 3\n"};
// A parameter row that is not the first (the first of an instrument page is at its limit for +1); a copied slot, step or song slot for the paste that follows; a changed value for the reset that follows.
const State kParamRow = {"cursor on the third parameter", Doc::Demo, "tap down 2\n"};
const State kSongClip = {"a song slot copied", Doc::Demo, "tap copy\ntap down\n"};
const State kInstClip = {"instrument 1 copied", Doc::Demo, "", "tap params\naction instrument.copy\ntap pattern\n"};
const State kClipCopied = {"edit on, a step copied", Doc::Demo, "", "tap rec\ntap copy\n"};
const State kMixTouched = {"a mixer value moved, a track muted", Doc::Demo, "action mixer.edit 3\naction mixer.mute\n"};
const State kStemsPage = {"the STEMS page", Doc::Demo, "tap f4\n"};
const State kToolValue = {"a tool value set", Doc::Demo, "action songtools.delta 1\n"};
const State kUnused = {"unused instruments", Doc::Sound, ""};
const State kName = {"a letter typed", Doc::Demo, "action text.pad 3\n"};
const State kLearn = {"two Learn bindings", Doc::DemoLearn, ""};
const State kSliced = {"sample loaded, one slice added", Doc::Sound, "action wave.add\n", "tap sampleload\nwait 120\ntap down 2\ntap f1\nwait 50\ntap f1\nwait 300\ntap pattern\n"};
const State kSel = {"edit on, 3 steps selected", Doc::Demo, "tap rec\npress shift\ntap down 3\nrelease shift\n"};
const State kClip = {"edit on, a step copied, cursor 2 down", Doc::Demo, "tap rec\ntap copy\ntap down 2\n"};

// The Sample load screen: the card has two folders (Projects, Samples) and then KICK.WAV and SNARE.WAV, so two steps down is KICK.WAV. The target is the instrument in view (instrument 1; 30 is free).
// A WAV onto a used sample slot (sound document), onto the module slot (tb303 document: instrument 1 is a module), onto a free slot.
const char* const kOnKick = "tap down 2\nassert state sel=KICK.WAV\n";
const State kLoadUsed = {"WAV onto a used sample slot", Doc::Sound, kOnKick};
const State kLoadModule = {"WAV onto a module slot", Doc::Tb303, kOnKick};
const State kLoadFree = {"WAV onto a free slot", Doc::Sound, "action instrument.select 30\ntap down 2\nassert state sel=KICK.WAV\n"};
const State kOnWav = {"cursor on a WAV", Doc::Sound, kOnKick};
const State kInFolder = {"inside a folder", Doc::Sound, "tap down\ntap f3\nwait 100\n"};
// The sample screens need audio in the slot: KICK.WAV loaded onto instrument 1 of the sound document before the screen is reached.
const State kLoaded = {"sample loaded", Doc::Sound, "", "tap sampleload\nwait 120\ntap down 2\ntap f1\nwait 50\ntap f1\nwait 300\ntap pattern\n"};

// OS-FWCONFIG: the simulator's update port (`firmware on`, four scripted files) behind Config's INFO page: F8 UPDATE opens the dialog; the dialog's own actions need the port attached to have a list to answer.
const State kFwPath = {"firmware update list open", Doc::Demo, ""};
const State kFwInfo = {"firmware update path attached, INFO page, project saved", Doc::Demo, "action file.save\nwait 300\ntap f4\n", "firmware on\n"};

}  // namespace

const std::vector<ActionEntry>& action_states() {
  static const std::vector<ActionEntry> table = {
      A("block.copy", {kEdit}),
      A("block.paste", {kClip}),
      A("chooser.coarse"),
      A("chooser.defined"),
      A("chooser.delta"),
      A("chooser.hold"),
      A("chooser.pad"),
      A("chooser.release"),
      A("chooser.toggle"),
      A("config.page_default"),
      A("cursor.jump_end"),
      A("cursor.jump_fraction"),
      A("cursor.page"),
      A("cursor.track"),
      A("dj.filter"),
      A("dj.fine"),
      A("dj.key"),
      A("dj.nudge"),
      A("dj.pad"),
      A("dj.tempo"),
      A("dj.turn"),
      A("edit.redo"),
      A("edit.undo"),
      A("export.cancel"),
      A("export.coarse"),
      A("export.value"),
      A("file.delete"),
      A("file.engine"),
      A("file.export"),
      A("file.name"),
      A("file.new"),
      A("file.open"),
      A("file.page"),
      A("file.purge", {kUnused}),
      A("file.report"),
      A("file.rescan"),
      A("file.save"),
      A("file.save_as"),
      A("fill.apply"),
      A("fill.coarse"),
      A("fill.delta"),
      A("fill.move"),
      A("fill.open"),
      A("firmware.back", {kFwPath}),
      A("firmware.go", {kFwPath}),
      A("firmware.open", {kFwInfo}),
      A("help.open"),
      A("input_test.reset"),
      A("instrument.copy"),
      A("instrument.delete"),
      A("instrument.init"),
      A("instrument.load"),
      A("instrument.name"),
      A("instrument.new_midi"),
      A("instrument.pad"),
      A("instrument.page"),
      A("instrument.paste", {kInstClip}),
      A("instrument.sample"),
      A("instrument.save"),
      A("instrument.select"),
      A("instrument.step"),
      A("instrument.turn"),
      A("instrument.used"),
      A("learn.clear", {kLearn}),
      A("learn.clear_all", {kLearn}),
      A("learn.delete", {kLearn}),
      A("learn.mode", {kLearn}),
      A("learn.toggle"),
      A("mixer.apply", {kStemsPage}),  // OS-USBOUTS: the simulator has no USB driver behind the board, so on STEMS it says NOTHING TO APPLY
      A("mixer.coarse"),
      A("mixer.cue"),
      A("mixer.edit"),
      A("mixer.master_vol"),
      A("mixer.mute"),
      A("mixer.mute_none", {kMixTouched}),
      A("mixer.name"),
      A("mixer.page"),
      A("mixer.reset", {kMixTouched}),
      A("mixer.solo"),
      A("mixer.solo_none", {kMixTouched}),
      A("mixer.stem_set"),
      A("module.app_edit"),
      A("module.app_reset"),
      A("module.engine"),
      A("module.exit"),
      A("module.launch"),
      A("module.pick"),
      A("module.rescan"),
      A("nav.down"),
      A("nav.left"),
      A("nav.peek"),
      A("nav.peek_end"),
      A("nav.right"),
      A("nav.screen"),
      A("nav.scroll"),
      A("nav.up"),
      A("navigator.accept"),
      A("navigator.open"),
      A("pad.audition"),
      A("pad.preview"),
      A("pads.mode"),
      A("pads.octave"),
      A("pads.play", {kEdit}),
      A("pads.track"),
      A("param.apply"),
      A("param.coarse", {kParamRow}),
      A("param.edit", {kParamRow}),
      A("param.group"),
      A("param.reset"),
      A("pattern.clip_copy"),
      A("pattern.clip_paste", {kClipCopied}),
      A("pattern.column"),
      A("pattern.edit_toggle"),
      A("pattern.fx_column", {kEdit}),
      A("pattern.invert", {kEdit, kSel}),
      A("pattern.preview"),
      A("pattern.rec_free"),
      A("pattern.zoom"),
      A("perform.copy"),
      A("perform.edit"),
      A("perform.fx"),
      A("perform.key"),
      A("perform.move"),
      A("perform.pad"),
      A("perform.paste"),
      A("perform.swap"),
      A("perform.turn"),
      A("popup.delta"),
      A("popup.move"),
      A("popup.open"),
      A("popup.pad"),
      A("popup.page"),
      A("popup.release"),
      A("preview.hold", {kLoaded}),
      A("record.coarse"),
      A("record.crop"),
      A("record.discard"),
      A("record.edit"),
      A("record.monitor"),
      A("record.pad"),
      A("record.preview"),
      A("record.reset"),
      A("record.save"),
      A("record.seek"),
      A("record.toggle"),
      A("record.wav"),
      A("sample.channel"),
      A("sample.enter"),
      A("sample.load", {kLoadUsed, kLoadModule, kLoadFree}),
      A("sample.mark"),
      A("sample.play", {kOnWav}),
      A("sample.rescan"),
      A("sample.target"),
      A("sample.up", {kInFolder}),
      A("sample.wt"),
      A("sedit.all", {kLoaded}),
      A("sedit.amiga", {kLoaded}),
      A("sedit.amp", {kLoaded}),
      A("sedit.fx", {kLoaded}),
      A("sedit.fx_coarse", {kLoaded}),
      A("sedit.instrument"),
      A("sedit.move", {kLoaded}),
      A("sedit.move_coarse", {kLoaded}),
      A("sedit.q8", {kLoaded}),
      A("sedit.redo", {kLoaded}),
      A("sedit.run", {kLoaded}),
      A("sedit.save_wav", {kLoaded}),
      A("sedit.undo", {kLoaded}),
      A("select.extend", {kEdit}),
      A("song.add_slot"),
      A("song.answer"),
      A("song.clear"),
      A("song.copy"),
      A("song.cut"),
      A("song.delete_slot"),
      A("song.move"),
      A("song.name"),
      A("song.next_use"),
      A("song.pad"),
      A("song.pads"),
      A("song.paste", {kSongClip}),
      A("song.play_pattern"),
      A("song.queue_jump"),
      A("song.select"),
      A("song.select_tracks"),
      A("song.tempo"),
      A("song.tempo_hold"),
      A("song.tools"),
      A("song.turn"),
      A("song.turn_fine"),
      A("song.unique"),
      A("songtools.apply", {kToolValue}),
      A("songtools.coarse"),
      A("songtools.delta"),
      A("songtools.move"),
      A("step.clear", {kEdit}),
      A("step.fx_pad", {kEdit}),
      A("step.fx_type_delta", {kEditFilled}),
      A("step.fx_value_coarse", {kEditFilled}),
      A("step.fx_value_delta", {kEditFilled}),
      A("step.insert", {kEdit}),
      A("step.instrument_coarse", {kEdit}),
      A("step.instrument_delta", {kEdit}),
      A("step.note_delta", {kEdit}),
      A("step.note_octave", {kEdit}),
      A("step.note_pad", {kEdit}),
      A("step.pull", {kEdit}),
      A("step.vol_coarse", {kEditFilled}),
      A("step.vol_delta", {kEditFilled}),
      A("step.vol_pad", {kEdit}),
      A("text.accept", {kName}),
      A("text.auto"),
      A("text.backspace"),
      A("text.caps"),
      A("text.key"),
      A("text.move"),
      A("text.pad"),
      A("text.scroll"),
      A("tools.open"),
      A("tools.run"),
      A("track.mute"),
      A("track.mute_mode"),
      A("track.solo"),
      A("transport.play"),
      A("transport.play_cursor"),
      A("ui.back"),
      A("ui.softkey"),
      A("ui.theme.next"),
      A("wave.add", {kLoaded}),
      A("wave.auto", {kLoaded}),
      A("wave.del", {kSliced}),
      A("wave.even", {kLoaded}),
      A("wave.even_n", {kLoaded}),
      A("wave.mode", {kLoaded}),
      A("wave.mode_pick"),
      A("wave.nudge", {kLoaded}),
      A("wave.nudge_coarse", {kLoaded}),
      A("wave.pad", {kLoaded}),
      A("wave.pad_select", {kLoaded}),
      A("wave.point", {kLoaded}),
      A("wave.zc", {kLoaded}),
      A("wave.zoom", {kLoaded}),
  };
  return table;
}

const std::vector<ContextReach>& context_reach() {
  static const std::vector<ContextReach> table = {
      {Context::Global, "Global", "", nullptr},
      {Context::Pattern, "Pattern", "", nullptr},
      {Context::PatternBrowse, "PatternBrowse", "", nullptr},
      {Context::PatternEdit, "PatternEdit", "tap rec\n", nullptr},
      {Context::PatternNote, "PatternNote", "", nullptr},
      {Context::PatternInstr, "PatternInstr", "tap instr\n", nullptr},
      {Context::PatternVol, "PatternVol", "tap vol\n", nullptr},
      {Context::PatternFx, "PatternFx", "tap fx\n", nullptr},
      {Context::Chooser, "Chooser", "press f1\nwait 500\n", nullptr},
      {Context::FillDialog, "FillDialog", "tap rec\ntap f4\nwait 20\n", nullptr},
      {Context::ToolsList, "ToolsList", "tap f7\n", nullptr},
      {Context::StepPopup, "StepPopup", "press note\nwait 500\n", nullptr},
      {Context::PadsMode, "PadsMode", "press shift\ntap f3\nrelease shift\n", nullptr},  // (PIANO: OS-PADOCT moved the mode keys one along)
      {Context::PadsTracks, "PadsTracks", "tap song\n", nullptr},
      {Context::Song, "Song", "tap song\n", nullptr},
      {Context::SongAsk, "SongAsk", "tap song\ntap right\ntap delete\nwait 20\n", nullptr},
      {Context::SongTools, "SongTools", "tap song\ntap f6\n", nullptr},
      {Context::Help, "Help", "press shift\ntap config\nrelease shift\n", nullptr},
      {Context::Navigator, "Navigator", "press shift\ntap pattern\nrelease shift\n", nullptr},
      {Context::Learn, "Learn", "action learn.toggle\n", nullptr},
      {Context::File, "File", "tap file\nwait 300\n", nullptr},
      {Context::FileExport, "FileExport", "tap file\nwait 300\ntap f7\n", nullptr},
      {Context::SampleLoad, "SampleLoad", "tap sampleload\nwait 300\n", nullptr},
      {Context::Wave, "Wave", "tap sampleplay\n", nullptr},
      {Context::WaveChop, "WaveChop", "tap sampleplay\ntap sampleplay\n", nullptr},
      {Context::PlayModePicker, "PlayModePicker", "tap sampleplay\npress shift\ntap f1\nrelease shift\n", nullptr},
      {Context::SampleEdit, "SampleEdit", "tap sampleedit\n", nullptr},
      {Context::SampleEditFx, "SampleEditFx", "tap sampleedit\ntap sampleedit\n", nullptr},
      {Context::Config, "Config", "tap config\n", nullptr},
      {Context::FirmwareUpdate, "FirmwareUpdate", "action file.save\nwait 300\ntap config\ntap f4\ntap f8\n", nullptr, "firmware on\n"},  // the update list over the simulator's update port; the seeded history left the project unsaved, which refuses the update (OS-FWCONFIG rule 2), so it is saved first
      {Context::Mixer, "Mixer", "tap master\n", nullptr},
      {Context::MixerStems, "MixerStems", nullptr, "the Mixer's page 3: the keys walk covers it as MIXER_page_3 (the simulator has no stem map, so its arrows have nothing to move here)"},
      {Context::Perform, "Perform", "tap performance\n", nullptr},
      {Context::PerformDj, "PerformDj", "tap performance\ntap performance\n", nullptr},
      {Context::Record, "Record", "tap samplerec\n", nullptr},
      {Context::RecordRadio, "RecordRadio", "tap samplerec\ntap samplerec\ntap samplerec\n", nullptr},
      {Context::Instrument, "Instrument", "tap params\n", nullptr},
      {Context::InstrumentEmpty, "InstrumentEmpty", "tap params\naction instrument.select 30\n", nullptr},
      {Context::Modules, "Modules", "apps on\naction nav.screen 12\nwait 100\n", nullptr},
      {Context::ModuleApp, "ModuleApp", "apps on\naction nav.screen 12\nwait 300\ntap down 2\ntap f1\nwait 300\n", nullptr},
      {Context::ModulePicker, "ModulePicker", "apps on\ntap params\naction instrument.select 30\ntap f4\n", nullptr},
      {Context::TextEntry, "TextEntry", "tap params\npress shift\ntap f1\nrelease shift\n", nullptr},
      {Context::WaveWt, "WaveWt", nullptr, "the Wave screen of a WAVETABLE instrument: no instrument of the two documents is one"},
      {Context::WaveGran, "WaveGran", nullptr, "the Wave screen of a GRANULAR instrument: no instrument of the two documents is one"},
      {Context::ExportProgress, "ExportProgress", "tap file\nwait 300\ntap f7\ntap f1\nwait 5\n", nullptr, "audio on\n"},
      {Context::InputTest, "InputTest", nullptr, "a hidden screen, reached only by the boot chord (Shift + F1 + F8 at power-on)"},
  };
  return table;
}

}  // namespace qanet
