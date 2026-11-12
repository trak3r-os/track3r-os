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
// OS-PTOOLS2 S3: the F7 verb menu open in STEP with a whole-step block selected, so the menu's pads act: the reach opens the menu, these
// steps close it (F8), turn STEP on, select the block and open it again — the pad pick then runs the verb under the cursor on the block.
const State kVerbMenuOpen = {"verb menu open in STEP, a block selected", Doc::Demo, "tap f8\ntap rec\npress shift\ntap down 3\nrelease shift\ntap f7\n"};
// OS-PTOOLS2 S2: the same, then the menu's pad 1 (TRANSPOSE) opened the Pattern TOOLS dialog over the block and AMOUNT was turned to +3, so
// ptool.apply has something to transpose. The steps close the reach's dialog (F8), keep Edit on (the reach's `tap rec` stands) and open it again.
const State kPToolsOpen = {"the Pattern TOOLS dialog open in STEP, a block selected, AMOUNT +3", Doc::Demo, "tap f8\npress shift\ntap down 3\nrelease shift\ntap f7\npad 0\ntap down\nencoder 3\n"};
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
// OS-LEARNMAP: the LEARN MAP overlay open over the two bindings (F6 MAP in the Learn dialog): the ENC row and two binding rows.
// (the context's reach opens the map: Learn, then F6; the cursor starts on the ENC row, which edits no document, so Down moves it to the first binding)
const State kLmap = {"LEARN MAP open over two bindings, cursor on the first", Doc::DemoLearn, "tap down\n"};
const State kSliced = {"sample loaded, one slice added", Doc::Sound, "action wave.add\n", "tap sampleload\nwait 120\ntap down 2\ntap f1\nwait 50\ntap f1\nwait 300\ntap pattern\n"};
const State kSel = {"edit on, 3 steps selected", Doc::Demo, "tap rec\npress shift\ntap down 3\nrelease shift\n"};
// OS-PTOOLS2 S3: edit on with a RANDOM fill made through the dialog (TYPE row to RANDOM), and the verb menu open (F7), so verb.repeat (F6 REPEAT) has a last verb whose repeat draws again and acts.
const State kFillDone = {"edit on, a random fill made on the selected track (the last verb is FILL), the verb menu open", Doc::Demo, "tap f8\ntap rec\naction step.clear\naction select.column_all\ntap f4\ntap down\naction fill.delta 1\ntap f1\ntap f7\n"};  // the leading F8 closes the menu that the VerbMenu reach opened
// OS-PTOOLS2 S1: the same, then the dialog reopened on the kept RANDOM rows, so F2 VARY takes the fill back and draws again (it acts).
const State kFillVary = {"the Fill dialog reopened on the random fill the reach set (F1 applies it, F4 opens it again)", Doc::Demo, "tap f1\ntap f4\n"};
const State kClip = {"edit on, a step copied, cursor 2 down", Doc::Demo, "tap rec\ntap copy\ntap down 2\n"};
// OS-MIDIREC S2b: the EVENTS overlay open with Edit on (STEP), so the field edits write; and the same with the cursor's
// field on the DATA of a NOTE row (the demo's first row), where a pad enters the note. The reach leaves the list open, so
// these close it (F8), turn STEP on (Rec) and open it again.
const State kEventsStep = {"events open in STEP", Doc::Demo, "tap f8\ntap rec\ntap f6\n"};
const State kEventsStepMicro = {"events open in STEP, the first row moved one microstep off its step", Doc::Demo, "tap f8\ntap rec\ntap f6\nencoder 1\n"};
const State kEventsStepData = {"events open in STEP, the DATA field of a NOTE row", Doc::Demo, "tap f8\ntap rec\ntap f6\ntap right 2\n"};

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

// ---- States that close the holes of tests/qanet/never_changed.txt: each puts in front of the action what it needs to act (OS-QANET A1 gap pass) -------------------------------------------------------
const State kDemo = {"demo", Doc::Demo, ""};
// The engine (`audio on`): the transport, the preview sink, the perform board and the mixer board exist only with it. Playing: the Play key has been pressed and the position published.
const char* const kAudioOn = "audio on\n";
const State kAudio = {"audio on", Doc::Demo, "", "", kAudioOn};
const State kAudioPlaying = {"audio on, playing", Doc::Demo, "tap play\nwait 60\n", "", kAudioOn};
// Perform: EDIT (Rec), a slot copied with the edit place on the next one, a slot with an FX whose row-1 pad is held (pad 12: slot 1, row 1; the encoder turns held slots with an FX on a row).
const State kPerfEdit = {"audio on, EDIT", Doc::Demo, "tap rec\n", "", kAudioOn};
const State kPerfCopied = {"audio on, EDIT, a slot copied, the edit place on the next", Doc::Demo, "tap rec\ntap copy\ntap right\n", "", kAudioOn};
const State kPerfHeld = {"audio on, a slot with an FX, its row-1 pad held", Doc::Demo, "tap rec\ntap up\ntap rec\npad 12 down\n", "", kAudioOn};
// The chooser of the pattern's LENGTH (F2 while F1's chooser is up): its places edit the pattern, which the PATTERN chooser (the reach) does not.
const State kLenChooser = {"LENGTH chooser open", Doc::Demo, "tap f2\n"};
// A pattern copied whole in the chooser (Copy), the chooser then on pattern 2 (pad 2), which the paste writes.
const State kPatCopied = {"edit on, chooser: pattern copied, on pattern 2", Doc::Demo, "tap copy\npad 1\n", "tap rec\n"};
// DJ page: the pattern row's pad 1 picks pattern 2, so that the next pad 1 (pattern 1) has a change to make.
const State kDjPattern = {"DJ page, pattern 2 in view", Doc::Demo, "action dj.pad 1\n"};
// EXPORT page on its PATTERN row with a second pattern: a second slot shares pattern 1 and song.unique makes pattern 2 of it as a copy (and the Pattern screen shows it), the PATTERN chooser (F1 held, pad 1) goes back to pattern 1, which the page opens on.
const State kExportPatterns = {"EXPORT page, PATTERN row, patterns 1 and 2", Doc::Demo, "tap down\n", "tap song\naction song.add_slot\naction song.unique\ntap pattern\npress f1\nwait 500\npad 0\nrelease f1\nwait 50\n"};
const State kImportPage = {"IMPORT page", Doc::Demo, "tap f6\nwait 50\n"};
// Learn on a screen with a focused parameter (Params: the first row), bound to a MIDI CC while the overlay is up; armed: the Clear-all key pressed once.
const State kLearnBound = {"Learn on Params, the focused parameter bound", Doc::Demo, "midicc 1 20 64\n", "tap params\n"};
const State kEncRow = {"Params, a learnable row focused", Doc::Demo, "", "tap params\n"};  // OS-ENCLEARN
const State kLearnArmed = {"Learn: two bindings, CLEAR ALL pressed once", Doc::DemoLearn, "action learn.clear_all\n"};
const State kLearnBoundArmed = {"Learn on Params: a binding, CLEAR ALL pressed once", Doc::Demo, "midicc 1 20 64\naction learn.clear_all\n", "tap params\n"};
// A track soloed, for the Mixer's SOLO NONE.
const State kMixSolo = {"a track soloed", Doc::Demo, "action mixer.solo\n"};
const State kPeek = {"Song held: peeking", Doc::Demo, "press song\nwait 500\n"};
const State kInstrument5 = {"instrument 5 in view", Doc::Demo, "action instrument.select 5\n"};
// STEP pads: a tap on an empty step writes the last note, so the first step of the cursor's track is cleared first.
const State kStepPads = {"edit on, first step cleared, STEP pads", Doc::Demo, "tap rec\naction step.clear\naction pads.mode 4\n"};
// The step under the cursor has an effect (a Roll in the column in view): the FX keys have a type to move and a value to set.
const State kEditFx = {"edit on, step with a Roll effect", Doc::Demo, "tap rec\naction step.fx_type 3\n"};
// The popup of another kind than the Note's (the reach opens the Note's): the Note's hold is let go, the button's is held, and the choice moved one place (the release enters it in Edit).
const State kPopupInstr = {"edit on, Instr list open, moved one place", Doc::Demo, "release note\npress instr\nwait 500\ntap down\n", "tap rec\n"};
const State kPopupFx = {"edit on, Fx list open, moved one place", Doc::Demo, "release note\npress fx\nwait 500\ntap down\n", "tap rec\n"};
const State kPopupVol = {"edit on, Vol list open, moved one place", Doc::Demo, "release note\npress vol\nwait 500\ntap down\n", "tap rec\n"};
// Song: two slots holding different patterns (the second is the cursor's), the cursor in the map, the pad layer on SLOTS, the song playing.
const State kSongTwo = {"two slots, patterns 1 and 2", Doc::Demo, "action song.add_slot\naction song.turn 1\n"};
const State kSongMap = {"cursor in the song map", Doc::Demo, "tap right\n"};
const State kSongTimelineTwo = {"TIMELINE page, two slots, patterns 1 and 2", Doc::Demo, "action song.add_slot\naction song.turn 1\ntap song\n"};  // OS-SONG3 S2: where Copy + Left / Right move the slot
// OS-SONG3 S4: F1 in a lane of the TIMELINE page is PREVIEW (the slot's pattern with the rectangle's tracks only): the cursor in a lane, the engine on so the transport exists.
const State kSongTimelineLane = {"TIMELINE, cursor in a lane, audio on", Doc::Demo, "tap song\ntap down\n", "", kAudioOn};
const State kSongSlotsPads = {"two slots, pads on SLOTS", Doc::Demo, "action song.add_slot\npress shift\ntap f7\nrelease shift\n"};
const State kSongTempoTap = {"F7 tapped once, 400 ms ago", Doc::Demo, "tap f7\nwait 400\n"};
// The Save As keyboard (the TextEntry reach opens the instrument's name: closed, then SAVE AS).
const State kSaveAsName = {"Save As name entry", Doc::Demo, "tap f8\naction file.save_as\nwait 50\n"};
// Sample screens: the Wave view zoomed in, a slice picked on the CHOP page, the play-mode list moved, the Sample edit selection moved, a sample edit done (and undone).
const char* const kLoadedPre = "tap sampleload\nwait 120\ntap down 2\ntap f1\nwait 50\ntap f1\nwait 300\ntap pattern\n";
const State kZoomedIn = {"sample loaded, zoomed in", Doc::Sound, "tap up\n", kLoadedPre};
const State kSliceOn = {"sample loaded, two slices, the second picked", Doc::Sound, "tap sampleplay\naction wave.add\ntap right\n", kLoadedPre};
const State kBeatSliced = {"sample loaded, EVEN made 16 slices (beat slice) on the CHOP page", Doc::Sound, "action wave.even\n", kLoadedPre};  // OS-UXF-SMP 3c: wave.to_pat writes them into the Pattern
const State kModeMoved = {"sample loaded, play-mode list moved", Doc::Sound, "tap down\n", kLoadedPre};
const State kSeditMoved = {"sample loaded, selection edge moved", Doc::Sound, "action sedit.move 1\n", kLoadedPre};
const State kSeditDone = {"sample loaded, an AMIGA edit made", Doc::Sound, "action sedit.amiga\nwait 600\n", kLoadedPre};
// OS-CEPTFX S4a: the PLUGINS page over the card's Effects/ (`card ptfx`: six listed plugins and a refused one), its quiet scan done; and the same with the first plugin's first parameter raised, so the resets have a value to bring back.
const State kPlugList = {"PLUGINS page, the scan done", Doc::Demo, "wait 800\n", "", "card ptfx\n"};
const State kPlugEdited = {"PLUGINS page, a parameter raised", Doc::Demo, "wait 800\ntap right\nencoder 3\n", "", "card ptfx\n"};
const State kSeditUndone = {"sample loaded, an AMIGA edit made and undone", Doc::Sound, "action sedit.amiga\nwait 600\naction sedit.undo\nwait 100\n", kLoadedPre};

}  // namespace

const std::vector<ActionEntry>& action_states() {
  static const std::vector<ActionEntry> table = {
      A("apps.close"),  // OS-APPSNAV: the apps list's Pattern; nothing open, so it answers false
      A("apps.open"),
      A("block.copy", {kEdit}),
      A("block.paste", {kClip}),
      A("chooser.coarse", {kDemo, kLenChooser}),
      A("chooser.defined", {kDemo, kLenChooser}),
      A("chooser.delta", {kDemo, kLenChooser}),
      A("chooser.hold"),
      A("chooser.pad", {kDemo, kLenChooser}),
      A("chooser.release"),
      A("chooser.toggle"),
      A("config.f8", {kFwInfo}),  // OS-LEARNMAP: MAP on the MIDI page, UPDATE on the INFO page (firmware.open's state)
      A("config.page_default"),
      A("cursor.jump_end"),
      A("cursor.jump_fraction"),
      A("cursor.page"),
      A("cursor.track"),
      A("dj.filter"),
      A("dj.fine"),
      A("dj.key"),
      A("dj.nudge"),
      A("dj.pad", {kDemo, kDjPattern}),
      A("dj.tempo"),
      A("dj.turn"),
      A("edit.redo"),
      A("edit.undo"),
      A("enc.assign", {kEncRow}),  // OS-ENCLEARN: Learn F7 ENC on a learnable row (the turn half, enc.turn, has no key of its own)
      A("events.back"),     // OS-MIDIREC S1b: the EVENTS overlay's actions (the walk opens it with Pattern F6, events.open)
      A("events.coarse", {kEventsStep}),  // S2b: the field edits write in STEP (the demo state says TURN EDIT ON in WATCH)
      A("events.delete", {kEventsStep}),  // S2c: Delete, A10
      A("events.edit", {kEventsStep}),
      A("events.field"),
      A("events.filter"),
      A("events.insert", {kEventsStep}),  // S2c: Insert / Shift + Insert, A9
      A("events.move"),
      A("events.open"),  // OS-F6EVENTS: Pattern F6 opens the list (it opens in WATCH too)
      A("events.page"),
      A("events.pad", {kEventsStepData}),  // S2b: a pad on the DATA field of a NOTE row enters the note (TURN EDIT ON in WATCH)
      A("events.preview"),
      A("events.quant", {kEventsStepMicro}),  // S3: F2 quantises the row the encoder just moved off its step
      A("events.redo", {kEventsStepMicro}),   // S2c: Shift + F7 inside the list (nothing to redo: it says so)
      A("events.undo", {kEventsStepMicro}),   // S2c: F7 inside the list takes the microstep back
      A("export.cancel"),
      A("export.coarse", {kDemo, kExportPatterns}),
      A("export.value", {kDemo, kExportPatterns}),
      A("file.delete"),
      A("file.engine"),
      A("file.export"),
      A("file.name"),
      A("file.new"),
      A("file.open"),
      A("file.page"),
      A("file.purge", {kUnused}),
      A("file.report", {kDemo, kImportPage}),
      A("file.rescan"),
      A("file.save"),
      A("file.save_any"),  // OS-UXF-KEYS item 1a: Shift + File, the same save as file.save
      A("file.save_as"),
      A("fill.apply"),
      A("fill.coarse"),
      A("fill.delta"),
      A("fill.move"),
      A("fill.open"),
      A("fill.vary", {kFillVary}),  // OS-PTOOLS2 S1: F2 in the dialog on a random fill
      A("firmware.back", {kFwPath}),
      A("firmware.go", {kFwPath}),
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
      A("instrument.select", {kDemo, kInstrument5}),
      A("instrument.step"),
      A("instrument.turn"),
      A("instrument.used"),
      A("learn.clear", {kLearn, kLearnBound}),
      A("learn.clear_all", {kLearn, kLearnArmed, kLearnBoundArmed}),
      A("learn.delete", {kLearn}),
      A("learn.follow", {kLearn, kLearnBound}),  // OS-LEARNFOLLOW S1: F5 in the Learn overlay
      A("learn.mode", {kLearn, kLearnBound}),
      A("learn.toggle"),
      A("lmap.coarse", {kLmap}),  // OS-LEARNMAP
      A("lmap.edit", {kLmap}),
      A("lmap.key", {kLmap}),
      A("lmap.open", {kLearn}),
      A("mixer.apply", {kStemsPage}),  // OS-USBOUTS: the simulator has no USB driver behind the board, so on STEMS it says NOTHING TO APPLY
      A("mixer.coarse"),
      A("mixer.cue"),
      A("mixer.edit"),
      A("mixer.inserts"),  // OS-MIXINSKEY: from the demo's screen it switches to the Mixer and sets INSERTS
      A("mixer.master_vol"),
      A("mixer.mute"),
      A("mixer.mute_none", {kMixTouched}),
      A("mixer.name"),
      A("mixer.page"),
      A("mixer.params"),  // OS-INSFX S5: on the INSERTS page Enter opens the insert's page; on the other pages it is the global nav.screen
      A("mixer.reset", {kMixTouched}),
      A("mixer.solo"),
      A("mixer.solo_none", {kMixTouched, kMixSolo}),
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
      A("nav.peek_end", {kDemo, kPeek}),
      A("nav.right"),
      A("nav.screen"),
      A("nav.scroll"),
      A("nav.up"),
      A("navigator.accept"),
      A("navigator.open"),
      A("pad.audition", {kDemo, kInstrument5}),
      A("pad.preview"),
      A("pads.octave"),
      A("pads.pick", {kDemo}),
      A("pads.picker"),
      A("pads.play", {kEdit, kStepPads}),
      A("pads.track"),
      A("param.apply"),
      A("param.coarse", {kParamRow}),
      A("param.edit", {kParamRow}),
      A("param.group"),
      A("param.reset"),
      A("pattern.clip_copy"),
      A("pattern.clip_paste", {kClipCopied, kPatCopied}),
      A("pattern.column"),
      A("pattern.edit_toggle"),
      A("pattern.fx_column", {kEdit}),
      A("pattern.preview"),  // (pattern.invert has no key since OS-PTOOLS2 S3: the verb menu's pad 9 runs it by name, so no binding reaches it and the table does not list it)
      A("pattern.rec_free", {kDemo, kAudio}),
      A("pattern.zoom"),
      A("ptool.move", {kPToolsOpen}),       // the dialog's rows
      A("ptool.delta", {kPToolsOpen}),      // the dialog's value row (Left / Right, the encoder)
      A("ptool.coarse", {kPToolsOpen}),     // Shift + the encoder: the coarse step (an octave on AMOUNT)
      A("ptool.apply", {kPToolsOpen}),      // F1 APPLY: one undo unit over the block (ptool.open itself has no key: the menu's rows dispatch it)
      A("perform.copy", {kDemo, kPerfEdit}),
      A("perform.edit", {kDemo, kAudio}),
      A("perform.fx", {kDemo, kPerfEdit}),
      A("perform.key", {kDemo, kAudio}),
      A("perform.move", {kDemo, kPerfEdit}),
      A("perform.pad", {kDemo, kAudio}),
      A("perform.paste", {kDemo, kPerfCopied}),
      A("perform.swap", {kDemo, kPerfEdit}),
      A("perform.turn", {kDemo, kPerfHeld}),
      A("popup.delta"),
      A("popup.move"),
      A("popup.open"),
      A("popup.pad", {kDemo, kPopupInstr, kPopupFx}),
      A("popup.page", {kDemo, kPopupInstr, kPopupFx}),
      A("popup.release", {kDemo, kPopupInstr, kPopupFx, kPopupVol}),
      A("preset.load"),  // (never_changed.txt: the list is reached only from a bound module's page)
      A("preset.save"),  // OS-MODPRESET P4: the list's Shift + F5 (never_changed.txt, like preset.load: no state of the walk's documents has a module bound)
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
      A("sedit.all", {kLoaded, kSeditMoved}),
      A("sedit.amiga", {kLoaded}),
      A("sedit.amp", {kLoaded}),
      A("sedit.fx", {kLoaded}),
      A("sedit.fx_coarse", {kLoaded}),
      A("sedit.instrument"),
      A("sedit.move", {kLoaded}),
      A("sedit.move_coarse", {kLoaded}),
      A("sedit.plug", {kPlugList}),
      A("sedit.plug_coarse", {kPlugList}),
      A("sedit.plug_do", {kPlugList, kPlugEdited}),
      A("sedit.q8", {kLoaded}),
      A("sedit.redo", {kLoaded, kSeditUndone}),
      A("sedit.run", {kLoaded}),
      A("sedit.save_wav", {kLoaded}),
      A("sedit.undo", {kLoaded, kSeditDone}),
      A("select.extend", {kEdit}),
      A("select.jump", {kEdit}),  // OS-SELSHIFT: the jump runs in every mode (cursor.jump_fraction has the plain state); the rows it crosses are selected only in Edit, the state run here
      A("song.add_slot"),
      A("song.add_empty", {kDemo, kSongTwo}),  // OS-SONG3 S5: Copy held + F3, a slot on the first free pattern
      A("song.answer"),
      A("song.clear"),
      A("song.copy"),
      A("song.cut"),
      A("song.delete_slot"),
      A("song.loop", {kDemo, kAudio}),
      A("song.move", {kDemo, kSongTwo}),
      A("song.move_along", {kSongTimelineTwo}),  // OS-SONG3 S2: the move along TIMELINE
      A("song.name"),
      A("song.next_use"),
      A("song.pad", {kDemo, kSongSlotsPads}),
      A("song.pads"),
      A("song.paste", {kSongClip}),
      A("song.play_pattern", {kDemo, kAudio, kSongTimelineLane}),  // OS-SONG3 S4: in a lane of TIMELINE, F1 is PREVIEW
      A("song.queue_jump", {kDemo, kAudioPlaying}),
      A("song.select"),
      A("song.select_tracks", {kDemo, kSongMap}),
      A("song.tempo", {kDemo, kSongTempoTap}),
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
      A("step.fx_pad", {kEdit, kEditFx}),
      A("step.fx_type_delta", {kEditFilled, kEditFx}),
      A("step.fx_value_coarse", {kEditFilled, kEditFx}),
      A("step.fx_value_delta", {kEditFilled, kEditFx}),
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
      A("text.auto", {kDemo, kSaveAsName}),
      A("text.backspace"),
      A("text.caps"),
      A("text.key"),
      A("text.move"),
      A("text.pad"),
      A("text.scroll"),
      A("verb.open"),  // OS-PTOOLS2 S3: F7, the verb menu (tools.open has no key any more: the menu's APPS > row runs it by name)
      A("verb.pick", {kVerbMenuOpen}),  // a pad 1-16 in the menu, arg = the pad's index (8 = INVERT); in this state pad 0 (TRANSPOSE) acts on the block
      A("verb.run", {kVerbMenuOpen}),   // F1 in the menu: the verb under the cursor
      A("verb.repeat", {kVerbMenuOpen, kFillDone}),  // OS-F6EVENTS: F6 in the menu closes it and runs tools.repeat (RE FILL / REPEAT)
      A("verb.hold", {kEdit, kSel}),    // OS-PTOOLS2 S3b: F7 held; opens nothing, it only marks the hold (refused like verb.open in LIVE / with no project)
      A("verb.hold_pad", {kEdit, kSel}),  // OS-PTOOLS2 S3b: F7 held + a pad, arg = the pad's index 0..15 (0 = pad 1, TRANSPOSE): the verb runs without the menu
      A("verb.hold_end", {kEdit, kSel}),  // OS-PTOOLS2 S3b: the release of a held F7 after its Hold: ends the hold, clears the pad mark
      A("track.mute"),
      A("track.mute_mode", {kDemo, kAudio}),
      A("track.solo"),
      A("transport.play", {kDemo, kAudio}),
      A("transport.play_cursor", {kDemo, kAudio}),
      A("ui.back"),
      A("ui.softkey"),
      A("ui.theme.next"),
      A("wave.add", {kLoaded}),
      A("wave.auto", {kLoaded}),
      A("wave.del", {kSliced}),
      A("wave.even", {kLoaded}),
      A("wave.even_n", {kLoaded}),
      A("wave.mode", {kLoaded}),
      A("wave.mode_pick", {kDemo, kModeMoved}),
      A("wave.nudge", {kLoaded}),
      A("wave.nudge_coarse", {kLoaded}),
      A("wave.pad", {kLoaded, kSliceOn}),
      A("wave.pad_select", {kLoaded, kSliceOn}),
      A("wave.point", {kLoaded}),
      A("wave.to_pat", {kBeatSliced}),
      A("wave.zc", {kLoaded}),
      A("wave.zoom", {kLoaded, kZoomedIn}),
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
      {Context::FillDialog, "FillDialog", "tap rec\naction select.column_all\ntap f4\ntap down\naction fill.delta 1\nwait 20\n", nullptr},  // the track selected (OS-PTOOLS2 S3c: the selection is the target) and the TYPE row (row 1 now, no SCOPE row) to RANDOM, so F2 VARY is on the key row (OS-PTOOLS2 S1) and acts
      {Context::VerbMenu, "VerbMenu", "tap f7\n", nullptr},  // OS-PTOOLS2 S3: F7 opens the verb menu
      {Context::PatternTools, "PatternTools", "tap rec\npress shift\ntap down 3\nrelease shift\ntap f7\npad 0\n", nullptr},  // OS-PTOOLS2 S2: the menu's pad 1 opens the TOOLS dialog over the block
      {Context::StepPopup, "StepPopup", "press note\nwait 500\n", nullptr},
      {Context::PadsMode, "PadsMode", "press shift\ntap f2\nrelease shift\ntap f2\n", nullptr},  // (PIANO, through the PADS picker: Shift + F2, then its F2; OS-UXF-PAT 7)
      {Context::PadsPicker, "PadsPicker", "press shift\ntap f2\nrelease shift\n", nullptr},  // OS-UXF-PAT 7
      {Context::PadsTracks, "PadsTracks", "tap song\n", nullptr},
      {Context::Song, "Song", "tap song\n", nullptr},
      {Context::SongAsk, "SongAsk", "tap song\ntap right\ntap delete\nwait 20\n", nullptr},
      {Context::SongTools, "SongTools", "tap song\ntap f6\n", nullptr},
      {Context::Help, "Help", "press shift\ntap config\nrelease shift\n", nullptr},
      {Context::Navigator, "Navigator", "press song\npress pattern\nrelease pattern\nrelease song\n", nullptr},  // OS-APPSNAV: the chord (Shift + Pattern lists the apps now)
      {Context::Learn, "Learn", "action learn.toggle\n", nullptr},
      {Context::LearnMap, "LearnMap", "action learn.toggle\ntap f6\n", nullptr},  // OS-LEARNMAP: F6 MAP in the Learn dialog
      {Context::File, "File", "tap file\nwait 300\n", nullptr},
      {Context::FileExport, "FileExport", "tap file\nwait 300\ntap f7\n", nullptr},
      {Context::SampleLoad, "SampleLoad", "tap sampleload\nwait 300\n", nullptr},
      {Context::Wave, "Wave", "tap sampleplay\n", nullptr},
      {Context::WaveChop, "WaveChop", "tap sampleplay\ntap sampleplay\n", nullptr},
      {Context::PlayModePicker, "PlayModePicker", "tap sampleplay\npress shift\ntap f1\nrelease shift\n", nullptr},
      {Context::SampleEdit, "SampleEdit", "tap sampleedit\n", nullptr},
      {Context::SampleEditFx, "SampleEditFx", "tap sampleedit\ntap sampleedit\n", nullptr},
      {Context::SampleEditPlug, "SampleEditPlug", "tap sampleedit\ntap sampleedit\ntap sampleedit\n", nullptr},
      {Context::Config, "Config", "tap config\n", nullptr},
      {Context::FirmwareUpdate, "FirmwareUpdate", "action file.save\nwait 300\ntap config\ntap f4\ntap f8\n", nullptr, "firmware on\n"},  // the update list over the simulator's update port; the seeded history left the project unsaved, which refuses the update (OS-FWCONFIG rule 2), so it is saved first
      {Context::Mixer, "Mixer", "tap master\n", nullptr},
      {Context::MixerStems, "MixerStems", nullptr, "the Mixer's page 3: the keys walk covers it as MIXER_page_3 (the simulator has no stem map, so its arrows have nothing to move here)", "", "tap master\ntap f4\n"},  // the rules walk reaches it on the STEMS page (F4): its one key (APPLY) answers NOTHING TO APPLY, there being no stem map and no USB driver
      {Context::MixerInserts, "MixerInserts", "tap master\ntap f6\n", nullptr},  // OS-INSKEYS: the INSERTS page (F6 from the Mixer); on it F6 is PARAMS, which on the demo document's empty cell says NO INSERT HERE
      {Context::InsertParams, "InsertParams", nullptr, "an insert's parameter page: no state of the walk's documents names an insert (the inserts document is only in qa_inserts.txt; F6 BACK and the blank keys are in tests/mixer_inserts_test.cpp, the context in builtin_bindings_test.cpp)"},  // OS-INSKEYS
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
      {Context::Preset, "Preset", nullptr, "the F5 PRESET list of a bound module's page: no state of the walk's documents has a module bound (the insert document's reverb is only in qa_presets.txt)"},
      {Context::WaveWt, "WaveWt", nullptr, "the Wave screen of a WAVETABLE instrument: no instrument of the two documents is one"},
      {Context::WaveGran, "WaveGran", nullptr, "the Wave screen of a GRANULAR instrument: no instrument of the two documents is one"},
      {Context::ExportProgress, "ExportProgress", "tap file\nwait 300\ntap f7\ntap f1\nwait 5\n", nullptr, "audio on\n"},
      {Context::InputTest, "InputTest", nullptr, "a hidden screen, reached only by the boot chord (Shift + F1 + F8 at power-on)", "", "action nav.screen 11\n"},  // the rules walk gets there through the screen registry
      {Context::Events, "Events", "tap f6\n", nullptr},  // OS-MIDIREC S1b, OS-F6EVENTS: Pattern F6 (EVENTS, A18) opens the list (it was F7, the verb menu, then F6)
  };
  return table;
}

}  // namespace qanet
