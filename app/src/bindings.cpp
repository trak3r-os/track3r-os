// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/bindings.h>

#include <o3os/app/actions.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/screens/ids.h>

namespace o3os::app {

namespace {

using core::ActionArg;
using core::ArgFrom;
using core::bind;
using core::chord;
using core::hold;
using core::hold_end;
using core::tap;

constexpr core::ContextId kGlobal = context_id(Context::Global);
constexpr core::ContextId kPattern = context_id(Context::Pattern);
constexpr core::ContextId kSong = context_id(Context::Song);
constexpr core::ContextId kLearnCtx = context_id(Context::Learn);
constexpr core::ContextId kNavigatorCtx = context_id(Context::Navigator);
constexpr core::ContextId kInputTestCtx = context_id(Context::InputTest);
constexpr core::ContextId kInstrumentCtx = context_id(Context::Instrument);
constexpr core::ContextId kInstrumentEmptyCtx = context_id(Context::InstrumentEmpty);  // OS-INSTUI S5
constexpr core::ContextId kBrowse = context_id(Context::PatternBrowse);
constexpr core::ContextId kNote = context_id(Context::PatternNote);
constexpr core::ContextId kInstr = context_id(Context::PatternInstr);
constexpr core::ContextId kVol = context_id(Context::PatternVol);
constexpr core::ContextId kFx = context_id(Context::PatternFx);
constexpr core::ContextId kChooserCtx = context_id(Context::Chooser);
constexpr core::ContextId kFileCtx = context_id(Context::File);
constexpr core::ContextId kFileExportCtx = context_id(Context::FileExport);
constexpr core::ContextId kExportProgressCtx = context_id(Context::ExportProgress);
constexpr core::ContextId kSampleLoadCtx = context_id(Context::SampleLoad);
constexpr core::ContextId kWaveCtx = context_id(Context::Wave);
constexpr core::ContextId kChopCtx = context_id(Context::WaveChop);
constexpr core::ContextId kWtCtx = context_id(Context::WaveWt);
constexpr core::ContextId kGranCtx = context_id(Context::WaveGran);
constexpr core::ContextId kPlayModePickerCtx = context_id(Context::PlayModePicker);
constexpr core::ContextId kRecordRadioCtx = context_id(Context::RecordRadio);
constexpr core::ContextId kSeditCtx = context_id(Context::SampleEdit);
constexpr core::ContextId kSeditFxCtx = context_id(Context::SampleEditFx);
constexpr core::ContextId kConfigCtx = context_id(Context::Config);
constexpr core::ContextId kMixerCtx = context_id(Context::Mixer);
constexpr core::ContextId kMixerStemsCtx = context_id(Context::MixerStems);  // OS-USBOUTS
constexpr core::ContextId kPerformCtx = context_id(Context::Perform);
constexpr core::ContextId kPerformDjCtx = context_id(Context::PerformDj);
constexpr core::ContextId kModulesCtx = context_id(Context::Modules);
constexpr core::ContextId kModuleAppCtx = context_id(Context::ModuleApp);
constexpr core::ContextId kModulePickerCtx = context_id(Context::ModulePicker);
constexpr core::ContextId kToolsCtx = context_id(Context::ToolsList);
constexpr core::ContextId kTextEntryCtx = context_id(Context::TextEntry);
constexpr core::ContextId kStepPopupCtx = context_id(Context::StepPopup);
constexpr core::ContextId kRecordCtx = context_id(Context::Record);
constexpr core::ContextId kPadsTracksCtx = context_id(Context::PadsTracks);
constexpr core::ContextId kPadsModeCtx = context_id(Context::PadsMode);
constexpr core::ContextId kFillCtx = context_id(Context::FillDialog);
constexpr core::ContextId kSongAskCtx = context_id(Context::SongAsk);
constexpr core::ContextId kSongToolsCtx = context_id(Context::SongTools);
constexpr core::ContextId kFirmwareCtx = context_id(Context::FirmwareUpdate);  // OS-FWCONFIG
constexpr core::ContextId kHelpCtx = context_id(Context::Help);

constexpr core::ActionId kPadAudition = core::action_id("pad.audition");  // OS-AUDIT-40, OS-AUDIT-26
constexpr core::ActionId kNavLeft = core::action_id("nav.left");
constexpr core::ActionId kNavRight = core::action_id("nav.right");
constexpr core::ActionId kNavUp = core::action_id("nav.up");
constexpr core::ActionId kNavDown = core::action_id("nav.down");
constexpr core::ActionId kNavScroll = core::action_id("nav.scroll");
constexpr core::ActionId kJumpFraction = core::action_id("cursor.jump_fraction");
constexpr core::ActionId kJumpEnd = core::action_id("cursor.jump_end");
constexpr core::ActionId kPage = core::action_id("cursor.page");
constexpr core::ActionId kTrack = core::action_id("cursor.track");
constexpr core::ActionId kNavScreen = core::action_id("nav.screen");
constexpr core::ActionId kNavPeek = core::action_id("nav.peek");            // OS-SONGPEEK
constexpr core::ActionId kNavPeekEnd = core::action_id("nav.peek_end");
constexpr core::ActionId kMixerMasterVol = core::action_id("mixer.master_vol");  // OS-MASTERVOL
constexpr core::ActionId kTempoHold = core::action_id("song.tempo_hold");  // OS-PATTEMPO
constexpr core::ActionId kRecFree = core::action_id("pattern.rec_free");
constexpr core::ActionId kNavigatorOpen = core::action_id("navigator.open");
constexpr core::ActionId kNavigatorAccept = core::action_id("navigator.accept");
constexpr core::ActionId kBack = core::action_id("ui.back");
constexpr core::ActionId kHelpOpen = core::action_id("help.open");  // OS-HELP
constexpr core::ActionId kInputTestReset = core::action_id("input_test.reset");
constexpr core::ActionId kColumn = core::action_id("pattern.column");
constexpr core::ActionId kFxColumn = core::action_id("pattern.fx_column");
constexpr core::ActionId kInstrument = core::action_id("instrument.select");
constexpr core::ActionId kInstrumentPage = core::action_id("instrument.page");  // OS-INSTUI S1/S2
constexpr core::ActionId kInstrumentTurn = core::action_id("instrument.turn");
constexpr core::ActionId kInstrumentStep = core::action_id("instrument.step");
constexpr core::ActionId kInstrumentUsed = core::action_id("instrument.used");
constexpr core::ActionId kInstrumentName = core::action_id("instrument.name");  // OS-INSTUI S7
constexpr core::ActionId kMixerName = core::action_id("mixer.name");  // OS-NAMES
constexpr core::ActionId kSongName = core::action_id("song.name");
constexpr core::ActionId kFileName = core::action_id("file.name");
constexpr core::ActionId kInstrumentCopy = core::action_id("instrument.copy");
constexpr core::ActionId kInstrumentPaste = core::action_id("instrument.paste");
constexpr core::ActionId kInstrumentSave = core::action_id("instrument.save");
constexpr core::ActionId kInstrumentLoad = core::action_id("instrument.load");
constexpr core::ActionId kInstrumentDelete = core::action_id("instrument.delete");  // OS-INSTUI S5
constexpr core::ActionId kInstrumentInit = core::action_id("instrument.init");
constexpr core::ActionId kInstrumentNewMidi = core::action_id("instrument.new_midi");
constexpr core::ActionId kInstrumentSample = core::action_id("instrument.sample");
constexpr core::ActionId kModuleEngine = core::action_id("module.engine");
constexpr core::ActionId kInstrumentPad = core::action_id("instrument.pad");
constexpr core::ActionId kSoftkey = core::action_id("ui.softkey");
constexpr core::ActionId kLearn = core::action_id("learn.toggle");
constexpr core::ActionId kThemeNext = core::action_id("ui.theme.next");
constexpr core::ActionId kUndo = core::action_id("edit.undo");
constexpr core::ActionId kRedo = core::action_id("edit.redo");
constexpr core::ActionId kParamEdit = core::action_id("param.edit");
constexpr core::ActionId kParamCoarse = core::action_id("param.coarse");
constexpr core::ActionId kParamReset = core::action_id("param.reset");
constexpr core::ActionId kParamGroup = core::action_id("param.group");
constexpr core::ActionId kParamApply = core::action_id("param.apply");
constexpr core::ActionId kLearnClear = core::action_id("learn.clear");
constexpr core::ActionId kLearnMode = core::action_id("learn.mode");
constexpr core::ActionId kLearnDelete = core::action_id("learn.delete");
constexpr core::ActionId kLearnClearAll = core::action_id("learn.clear_all");
// OS-009 P1a.1: the pattern's edit core.
constexpr core::ActionId kEditToggle = core::action_id("pattern.edit_toggle");
constexpr core::ActionId kPreview = core::action_id("pattern.preview");
constexpr core::ActionId kPatternZoom = core::action_id("pattern.zoom");  // OS-ZOOM
constexpr core::ActionId kFillOpen = core::action_id("fill.open");
constexpr core::ActionId kFillMove = core::action_id("fill.move");
constexpr core::ActionId kFillDelta = core::action_id("fill.delta");
constexpr core::ActionId kFillCoarse = core::action_id("fill.coarse");  // OS-SHIFTENC
constexpr core::ActionId kFillApply = core::action_id("fill.apply");
constexpr core::ActionId kPatternInvert = core::action_id("pattern.invert");
constexpr core::ActionId kPadsMode = core::action_id("pads.mode");
constexpr core::ActionId kPadsOctave = core::action_id("pads.octave");  // OS-PADOCT
constexpr core::ActionId kPadsTrack = core::action_id("pads.track");
constexpr core::ActionId kPadsPlay = core::action_id("pads.play");
constexpr core::ActionId kPadPreview = core::action_id("pad.preview");
constexpr core::ActionId kTransportPlay = core::action_id("transport.play");
constexpr core::ActionId kTransportPlayCursor = core::action_id("transport.play_cursor");  // OS-PLAYCUR
constexpr core::ActionId kSelectExtend = core::action_id("select.extend");
constexpr core::ActionId kStepNotePad = core::action_id("step.note_pad");
constexpr core::ActionId kStepNoteDelta = core::action_id("step.note_delta");
constexpr core::ActionId kStepNoteOctave = core::action_id("step.note_octave");
constexpr core::ActionId kStepInstrumentDelta = core::action_id("step.instrument_delta");
constexpr core::ActionId kStepInstrumentCoarse = core::action_id("step.instrument_coarse");  // OS-SHIFTENC
constexpr core::ActionId kStepVolPad = core::action_id("step.vol_pad");
constexpr core::ActionId kStepVolDelta = core::action_id("step.vol_delta");
constexpr core::ActionId kStepVolCoarse = core::action_id("step.vol_coarse");  // OS-SHIFTENC
constexpr core::ActionId kStepFxTypeDelta = core::action_id("step.fx_type_delta");
constexpr core::ActionId kStepFxValueDelta = core::action_id("step.fx_value_delta");
constexpr core::ActionId kStepFxValueCoarse = core::action_id("step.fx_value_coarse");  // OS-SHIFTENC
constexpr core::ActionId kStepFxPad = core::action_id("step.fx_pad");
constexpr core::ActionId kStepClear = core::action_id("step.clear");
constexpr core::ActionId kBlockCopy = core::action_id("block.copy");
constexpr core::ActionId kBlockPaste = core::action_id("block.paste");
constexpr core::ActionId kPatternClipCopy = core::action_id("pattern.clip_copy");    // PAT-OPS-B
constexpr core::ActionId kPatternClipPaste = core::action_id("pattern.clip_paste");  // PAT-OPS-B
constexpr core::ActionId kStepInsert = core::action_id("step.insert");
constexpr core::ActionId kStepPull = core::action_id("step.pull");
// OS-SONG1: the Song screen's editing.
constexpr core::ActionId kSongAdd = core::action_id("song.add_slot");
constexpr core::ActionId kSongDelete = core::action_id("song.delete_slot");
constexpr core::ActionId kSongPatternDelta = core::action_id("song.pattern_delta");
constexpr core::ActionId kSongTurn = core::action_id("song.turn");
constexpr core::ActionId kSongTurnFine = core::action_id("song.turn_fine");
constexpr core::ActionId kSongTempo = core::action_id("song.tempo");
constexpr core::ActionId kSongSelect = core::action_id("song.select");
constexpr core::ActionId kSongCopy = core::action_id("song.copy");
constexpr core::ActionId kSongPaste = core::action_id("song.paste");
constexpr core::ActionId kSongPlayPattern = core::action_id("song.play_pattern");
constexpr core::ActionId kSongQueueJump = core::action_id("song.queue_jump");
constexpr core::ActionId kSongPads = core::action_id("song.pads");  // OS-SONGPADS
constexpr core::ActionId kSongClear = core::action_id("song.clear");  // OS-SONGMAP
constexpr core::ActionId kSongCut = core::action_id("song.cut");  // OS-SONGSHARE S2
constexpr core::ActionId kSongMove = core::action_id("song.move");  // OS-SONGSHARE S3
constexpr core::ActionId kSongNextUse = core::action_id("song.next_use");  // OS-SONGSHARE S4
constexpr core::ActionId kSongSelectTracks = core::action_id("song.select_tracks");
constexpr core::ActionId kSongUnique = core::action_id("song.unique");
constexpr core::ActionId kSongTools = core::action_id("song.tools");
constexpr core::ActionId kSongAnswer = core::action_id("song.answer");
constexpr core::ActionId kSongToolsMove = core::action_id("songtools.move");
constexpr core::ActionId kSongToolsDelta = core::action_id("songtools.delta");
constexpr core::ActionId kSongToolsCoarse = core::action_id("songtools.coarse");
constexpr core::ActionId kSongToolsApply = core::action_id("songtools.apply");
constexpr core::ActionId kSongPad = core::action_id("song.pad");
constexpr core::ActionId kTrackMute = core::action_id("track.mute");
constexpr core::ActionId kTrackMuteMode = core::action_id("track.mute_mode");
constexpr core::ActionId kTrackSolo = core::action_id("track.solo");
constexpr core::ActionId kChooserToggle = core::action_id("chooser.toggle");
constexpr core::ActionId kChooserHold = core::action_id("chooser.hold");
constexpr core::ActionId kChooserRelease = core::action_id("chooser.release");
constexpr core::ActionId kChooserDelta = core::action_id("chooser.delta");
constexpr core::ActionId kChooserCoarse = core::action_id("chooser.coarse");  // OS-SHIFTENC
constexpr core::ActionId kChooserDefined = core::action_id("chooser.defined");
constexpr core::ActionId kChooserPad = core::action_id("chooser.pad");
constexpr core::ActionId kPopupOpen = core::action_id("popup.open");
constexpr core::ActionId kPopupRelease = core::action_id("popup.release");
constexpr core::ActionId kPopupMove = core::action_id("popup.move");
constexpr core::ActionId kPopupDelta = core::action_id("popup.delta");
constexpr core::ActionId kPopupPage = core::action_id("popup.page");
constexpr core::ActionId kPopupPad = core::action_id("popup.pad");
constexpr core::ActionId kPreviewHold = core::action_id("preview.hold");
constexpr core::ActionId kRecordPreview = core::action_id("record.preview");
constexpr core::ActionId kFileOpen = core::action_id("file.open");
constexpr core::ActionId kFileRescan = core::action_id("file.rescan");
constexpr core::ActionId kFileNew = core::action_id("file.new");
constexpr core::ActionId kFilePage = core::action_id("file.page");
constexpr core::ActionId kFileEngine = core::action_id("file.engine");
constexpr core::ActionId kFileReport = core::action_id("file.report");
constexpr core::ActionId kFileDelete = core::action_id("file.delete");
constexpr core::ActionId kFilePurge = core::action_id("file.purge");
constexpr core::ActionId kFileSave = core::action_id("file.save");
constexpr core::ActionId kFileSaveAs = core::action_id("file.save_as");
constexpr core::ActionId kSeditSaveWav = core::action_id("sedit.save_wav");
constexpr core::ActionId kSampleLoad = core::action_id("sample.load");
constexpr core::ActionId kSampleEnter = core::action_id("sample.enter");
constexpr core::ActionId kSampleUp = core::action_id("sample.up");
constexpr core::ActionId kSampleRescan = core::action_id("sample.rescan");
constexpr core::ActionId kSamplePlay = core::action_id("sample.play");
constexpr core::ActionId kSampleWt = core::action_id("sample.wt");
constexpr core::ActionId kSampleChannel = core::action_id("sample.channel");
constexpr core::ActionId kSampleTarget = core::action_id("sample.target");
constexpr core::ActionId kSampleMark = core::action_id("sample.mark");
constexpr core::ActionId kTextPad = core::action_id("text.pad");
constexpr core::ActionId kTextMove = core::action_id("text.move");
constexpr core::ActionId kTextScroll = core::action_id("text.scroll");
constexpr core::ActionId kTextKey = core::action_id("text.key");
constexpr core::ActionId kTextCaps = core::action_id("text.caps");
constexpr core::ActionId kTextBackspace = core::action_id("text.backspace");
constexpr core::ActionId kTextAuto = core::action_id("text.auto");
constexpr core::ActionId kTextAccept = core::action_id("text.accept");
constexpr core::ActionId kWaveNudge = core::action_id("wave.nudge");
constexpr core::ActionId kWaveNudgeCoarse = core::action_id("wave.nudge_coarse");
constexpr core::ActionId kWavePoint = core::action_id("wave.point");
constexpr core::ActionId kWaveZoom = core::action_id("wave.zoom");
constexpr core::ActionId kWaveZc = core::action_id("wave.zc");
constexpr core::ActionId kWaveEvenN = core::action_id("wave.even_n");
constexpr core::ActionId kWaveAdd = core::action_id("wave.add");
constexpr core::ActionId kWaveDel = core::action_id("wave.del");
constexpr core::ActionId kWaveEven = core::action_id("wave.even");
constexpr core::ActionId kWaveAuto = core::action_id("wave.auto");
constexpr core::ActionId kFileExport = core::action_id("file.export");              // OS-WAVEXP
constexpr core::ActionId kExportValue = core::action_id("export.value");
constexpr core::ActionId kExportCoarse = core::action_id("export.coarse");  // OS-SHIFTENC
constexpr core::ActionId kExportCancel = core::action_id("export.cancel");
constexpr core::ActionId kWaveMode = core::action_id("wave.mode");            // OS-INSTUI S4
constexpr core::ActionId kWaveModePick = core::action_id("wave.mode_pick");
constexpr core::ActionId kWavePad = core::action_id("wave.pad");
constexpr core::ActionId kWavePadSelect = core::action_id("wave.pad_select");
constexpr core::ActionId kSeditMove = core::action_id("sedit.move");
constexpr core::ActionId kSeditMoveCoarse = core::action_id("sedit.move_coarse");
constexpr core::ActionId kSeditFx = core::action_id("sedit.fx");
constexpr core::ActionId kSeditFxCoarse = core::action_id("sedit.fx_coarse");
constexpr core::ActionId kSeditAll = core::action_id("sedit.all");
constexpr core::ActionId kSeditAmp = core::action_id("sedit.amp");
constexpr core::ActionId kSeditRun = core::action_id("sedit.run");
constexpr core::ActionId kSeditAmiga = core::action_id("sedit.amiga");
constexpr core::ActionId kSeditQ8 = core::action_id("sedit.q8");
constexpr core::ActionId kSeditInstrument = core::action_id("sedit.instrument");
constexpr core::ActionId kSeditUndo = core::action_id("sedit.undo");
constexpr core::ActionId kSeditRedo = core::action_id("sedit.redo");
constexpr core::ActionId kMixerEdit = core::action_id("mixer.edit");
constexpr core::ActionId kMixerCoarse = core::action_id("mixer.coarse");
constexpr core::ActionId kMixerReset = core::action_id("mixer.reset");
constexpr core::ActionId kMixerStemSet = core::action_id("mixer.stem_set");
constexpr core::ActionId kMixerMute = core::action_id("mixer.mute");
constexpr core::ActionId kMixerSolo = core::action_id("mixer.solo");
constexpr core::ActionId kMixerPage = core::action_id("mixer.page");
constexpr core::ActionId kMixerCue = core::action_id("mixer.cue");
constexpr core::ActionId kMixerApply = core::action_id("mixer.apply");  // OS-USBOUTS
constexpr core::ActionId kPerformPad = core::action_id("perform.pad");
constexpr core::ActionId kPerformTurn = core::action_id("perform.turn");
constexpr core::ActionId kPerformKey = core::action_id("perform.key");
constexpr core::ActionId kPerformEdit = core::action_id("perform.edit");
constexpr core::ActionId kPerformMove = core::action_id("perform.move");
constexpr core::ActionId kPerformSwap = core::action_id("perform.swap");
constexpr core::ActionId kPerformFx = core::action_id("perform.fx");
constexpr core::ActionId kPerformCopy = core::action_id("perform.copy");
constexpr core::ActionId kPerformPaste = core::action_id("perform.paste");
constexpr core::ActionId kDjPad = core::action_id("dj.pad");
constexpr core::ActionId kDjTurn = core::action_id("dj.turn");
constexpr core::ActionId kDjFine = core::action_id("dj.fine");
constexpr core::ActionId kDjKey = core::action_id("dj.key");
constexpr core::ActionId kDjFilter = core::action_id("dj.filter");
constexpr core::ActionId kDjNudge = core::action_id("dj.nudge");
constexpr core::ActionId kDjTempo = core::action_id("dj.tempo");
// OS-012b: the SD modules.
constexpr core::ActionId kModulePick = core::action_id("module.pick");
constexpr core::ActionId kToolsOpen = core::action_id("tools.open");  // OS-H3A
constexpr core::ActionId kToolsRun = core::action_id("tools.run");
constexpr core::ActionId kModuleRescan = core::action_id("module.rescan");
constexpr core::ActionId kModuleLaunch = core::action_id("module.launch");
constexpr core::ActionId kModuleExit = core::action_id("module.exit");
constexpr core::ActionId kModuleAppEdit = core::action_id("module.app_edit");
constexpr core::ActionId kModuleAppReset = core::action_id("module.app_reset");
constexpr core::ActionId kRecordEdit = core::action_id("record.edit");
constexpr core::ActionId kRecordCoarse = core::action_id("record.coarse");
constexpr core::ActionId kRecordReset = core::action_id("record.reset");
constexpr core::ActionId kRecordToggle = core::action_id("record.toggle");
constexpr core::ActionId kRecordPad = core::action_id("record.pad");
constexpr core::ActionId kRecordMonitor = core::action_id("record.monitor");
constexpr core::ActionId kRecordSave = core::action_id("record.save");
constexpr core::ActionId kRecordDiscard = core::action_id("record.discard");
constexpr core::ActionId kRecordCrop = core::action_id("record.crop");
constexpr core::ActionId kRecordSeek = core::action_id("record.seek");
constexpr core::ActionId kRecordWav = core::action_id("record.wav");
constexpr core::ActionId kMixerMuteNone = core::action_id("mixer.mute_none");  // OS-SHIFTROW
constexpr core::ActionId kMixerSoloNone = core::action_id("mixer.solo_none");
constexpr core::ActionId kConfigPageDefault = core::action_id("config.page_default");
constexpr core::ActionId kFirmwareOpen = core::action_id("firmware.open");  // OS-FWCONFIG
constexpr core::ActionId kFirmwareGo = core::action_id("firmware.go");
constexpr core::ActionId kFirmwareBack = core::action_id("firmware.back");
constexpr core::ActionId kUiNone = core::action_id("ui.none");  // the registered do-nothing action: a binding to it ends the resolver's walk with nothing

constexpr ActionArg column_arg(Column c) noexcept { return static_cast<ActionArg>(c); }
constexpr ActionArg screen_arg(ScreenId s) noexcept { return static_cast<ActionArg>(s); }

constexpr core::InputRef kShift = button_ref(Button::Shift);
// OS-ZOOM: the one bound modifier, Note (core/gesture.h, "Held modifiers"): not in kModifierButtons (the recogniser's own, Shift), a held modifier only where a binding of the chain names it (kKeymapModifiers, below, lets a keymap line name it too).
constexpr core::InputRef kNoteKey = button_ref(Button::Note);
// OS-SONGSHARE S3: Copy is the second held modifier, on Song only (core/gesture.h, "Held modifiers"): Copy + Up / Down moves the slots. Its own Tap, and Shift + Copy's, then wait for the release; Copy followed by another button is consumed.
constexpr core::InputRef kCopyKey = button_ref(Button::Copy);

// The device's built-in bindings. A context's keys are its own: the walk of the chain stops at the first context that
// binds a gesture (core/binding.h), so what is bound in Global is only what no screen or overlay claims.
//
// Shift combinations, all of them (the conflict check of OS-002b; builtin_bindings_test.cpp keeps this list honest):
//   Shift + Master       ui.theme.next      Global     (OS-003b, provisional until Config exists)
//   Shift + Rec          pattern.rec_free   Global     LIVE recording on / off (OS-RECMODE2: stopped it starts the transport, with the count-in; the plain Rec, pattern.edit_toggle, is STEP on / off); on a screen but Pattern it does nothing
//   Shift + Pattern      navigator.open     Global     (OS-NAV2: tapped or held, the Navigator stays open until F8 GO / F7 CLOSE; the plain Pattern is the Pattern button)
//   Shift + Config       help.open          Global and ModuleApp (OS-HELP: the help page of the screen in view; a modal overlay's context captures it, so under a dialog it does nothing; the app page binds it itself; the plain Config is the Config screen)
//   Shift + F8           edit.redo          Pattern and Song (OS-002c; the plain F8 there is edit.undo)
//   Shift + F1..F8       ui.none           Global     (OS-SHIFTROW: Shift + F belongs to the screen; the quarter jumps, cursor.jump_fraction 0..3 and cursor.jump_end, are bound in Song, File and Sample load;
//                        on Pattern the pads' mode keys, below). Also: Shift + F1 in Mixer mixer.name, Shift + F6 in Song and File song.name / file.name (OS-NAMES), Shift + F7 / F8 in Mixer mixer.mute_none / solo_none, Shift + F1..F3 in Perform perform.key 0x100.. (STORE), Shift + F7 in Config
//                        config.page_default, Shift + F5 / F6 in Record (RADIO page only) record.seek -10 / 10, Shift + F7 in Song song.pads (OS-SONGPADS: the pads' TRACKS / SLOTS), Shift + F1 in the firmware update dialog ui.none (OS-FWCONFIG: only F1 alone answers)
//   Shift + F1         pads.octave        Pattern (OS-PADOCT: the pads' base C goes up an octave, 9 wraps to 0)
//   Shift + F2..F7       pads.mode 0..6     Pattern (OS-PADS-A / B / PADOCT: F2 NOTES, F3 PIANO, F4 ISO, F5 TRACKS, F6 PATS, F7 VU; STEP has no key: OS-STEPPADOFF)
//   Shift + Note / Instr / Vol / Fx   cursor.jump_fraction 0..3   Pattern (OS-QJUMP: the quarter jumps, 0, 1/4, 2/4, 3/4 of the pattern; Shift + Fx was pattern.fill, which has no key now: the Fill dialog is F4)
//   Shift + encoder      param.coarse       Instrument (OS-002d; the plain encoder is param.edit there)
//   Shift + F8           edit.redo          also Instrument
//   Shift + Left / Right param.coarse -1 / 1  Instrument (OS-INSTUI-ARROWS: the coarse step; the plain arrows are param.edit -1 / 1 there; the empty slot's chooser keeps instrument.turn) and Config (OS-CFG-ARROWS)
//   Shift + Left / Right record.coarse -1 / 1 Record (OS-CFG-ARROWS: the coarse step; the plain arrows are record.edit -1 / 1)
//   Shift + F6           sedit.save_wav     Sample edit (OS-007n: the sample as a WAV in Samples/; the key row is full; OS-FU3 moved it from Shift + F7, which plays from the edge)
//   Shift + F1 / F2 / F3 sedit.amiga 0 / 1, sedit.q8   Sample edit (OS-AMG1: the AMIGA conversion at 8,287 / 16,574 Hz and its 8-bit option)
//   Shift + F4           sedit.run 7        Sample edit (OS-SEDFX: DC REMOVE, EditOp::DcRemove)
//   Shift + encoder      sedit.fx_coarse    Sample edit, FX page (OS-AUDIT-15: the row's value in coarse steps; the plain encoder is sedit.fx)
//   Shift + F5           sedit.instrument   Sample edit (OS-QAFIX-27: NEXT INS, the next instrument to edit, round; a pad picks one too)
//   Shift + arrows       select.extend      Pattern (OS-009: the block; Shift + Up at step 0 selects the column, again every track)
//   Note + Instr / Left / Right   pattern.zoom / cursor.track -1 / 1   Pattern and its step popup (OS-ZOOM: Note is a held modifier, core/gesture.h; not a Shift row, tests/builtin_bindings_test.cpp lists these too)
//   Shift + Insert       cursor.jump_fraction 0   Pattern (HOME: collapses the block and goes to step 0)
//   Shift + Delete       step.pull          Pattern (OS-DELPULL: the inverse of Insert)
//   Copy / Shift + Copy  block.copy / block.paste   Pattern (OS-PATCOPY: the block into the clipboard, the clipboard at the cursor)
//   Shift + Copy         pattern.clip_paste  Chooser (PAT-OPS-B: with F1 held on Pattern, the whole pattern of the clipboard over the chooser's pattern; the plain Copy there is pattern.clip_copy)
//   Shift + Play         transport.play_cursor   Pattern (OS-PLAYCUR: stopped, play from the cursor's row; playing, the line jumps to it; never a count-in; the Focus Loop's key, which it keeps until the loop exists); other screens: the plain Play
//   Shift + Insert       song.add_slot 1    Song (OS-SONG1: the slot with a copy of the pattern; OS-SHIFTROW moved it from Shift + F3, which is the 2/4 jump again)
//   Shift + Up / Down    cursor.page -1 / 1 File (PROJECTS and IMPORT), Sample load, Modules, the engine picker, Learn, the help page (OS-NAV3, OS-HELP: a screenful; a page being the rows the list shows; popup.page in the step popup's instrument and FX lists); never wraps
//   Shift + Up / Down    song.select        Song (OS-SONG1: the range of slots), Shift + encoder song.turn_fine, Shift + Copy song.paste
//   Shift + Right / Left sample.mark 0 / 1  Sample load (OS-AUDIT-A7, SLD-MULTI: mark the file under the cursor and step down / clear the marks; F1 / F4 then load the marked files into free instruments)
//   Shift + Left / Right song.select_tracks Song (OS-SONGMAP: the range of tracks, in the map only; on PAT it does nothing), Copy / Shift + Copy / Delete act on the map's cells while the cursor is in it
//   Copy + Up / Down     song.move -1 / 1   Song (OS-SONGSHARE S3: Copy is a held modifier on Song, Copy + Up / Down moves the range of slots one position; not a Shift row, tests/builtin_bindings_test.cpp lists these too)
//   Shift + Note         song.next_use -1   Song (OS-SONGSHARE S4: the previous slot that holds the cursor's pattern; the plain Note is the next one; both wrap; on Pattern the same chord is the 0/4 jump)
//   Shift + Delete       song.cut           Song (OS-SONGSHARE S2: CUT; on PAT the slots into the slot clipboard and out of the list, in the map the cells into the song clip and cleared; it fell back to the plain Delete before)
//   Shift + encoder      OS-SHIFTENC (coarse step where a value or a list had no Shift meaning): Pattern Instr step.instrument_coarse (x8 places), Vol step.vol_coarse (x10), Fx step.fx_value_coarse (x16, one nibble);
//                        cursor.page (a detent a page) in Pattern Browse, File, Sample load, Modules, the Navigator, Learn, the engine picker, TOOLS and the play-mode picker; popup.page in the step popup;
//                        export.coarse (x8 patterns), chooser.coarse (x8 places), fill.coarse (the row's coarse step). Unchanged: Note (octave), Song (fine) and every context that had one
//   Shift + Delete       mixer.reset 1      Mixer (STEMS: the factory map; the plain Delete there clears the source, Insert sets it)
// Against the code there is no conflict (find_conflict above, and the test resolves each in its own chain). Shift +
// arrows, Play, Copy, Insert and Delete, which the pattern editor could use for ranges, paste and the like, were left
// unbound, so a held Shift still falls through to the plain key; OS-009 binds the ones of P1a.1 (above), Shift + Play and Copy wait for
// P1b and P1d (the plain Play is the transport, OS-005d). Shift + F1..F8 were the track mutes 1-8 (product-ux.md section 3); the maintainer took
// F1..F5 for the jumps and moved mute and solo to Shift + the pads (orchestrator decisions 2026-10-11); OS-PADS-A moved them on to the TRACKS layer of the pads (a lone Shift hold or
// Shift + F3, pad_layer.h), which takes the pads' place: no Shift + pad binding is left.
constexpr core::Binding kBase[] = {
    // ---- Global: the screen buttons, the Navigator, Learn, theme, mode -------------------------------------------------
    bind(kGlobal, tap(button_ref(Button::Pattern)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Pattern)),
    bind(kGlobal, tap(button_ref(Button::Song)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Song)),
    bind(kGlobal, tap(button_ref(Button::Params)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Instrument)),
    bind(kGlobal, tap(button_ref(Button::SamplePlay)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Wave)),
    bind(kGlobal, tap(button_ref(Button::SampleEdit)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::SampleEdit)),
    bind(kGlobal, tap(button_ref(Button::SampleLoad)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::SampleLoad)),
    bind(kGlobal, tap(button_ref(Button::SampleRec)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Record)),
    bind(kGlobal, tap(button_ref(Button::Master)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Mixer)),
    // OS-MASTERVOL: Master held + the encoder moves the master volume, on any screen (the turn consumes the Master tap; the Tap of Master and of Shift + Master therefore comes at the release, not
    // at the press). OS-SONGPEEK: Song held shows Song until the release, which goes one level back; the Tap of Song also comes at the release now, and a hold of 400 ms is the peek, not a tap.
    bind(kGlobal, hold_encoder(encoder_ref(0), button_ref(Button::Master)), kMixerMasterVol, ArgFrom::Value),
    // OS-PATTEMPO (NEW): Pattern held + the encoder is the project tempo, on any screen (a pad under the held key is a tempo tap, taken by the App ahead of the bindings: app.cpp). The turn or the tap consumes the
    // Pattern tap, so the plain Pattern, and Shift + Pattern (the Navigator), tap at the release too. Pattern F1 + the encoder (OS-PATSWITCH) is F1's. A capturing context (overlays, an app's page) never reaches this.
    bind(kGlobal, hold_encoder(encoder_ref(0), button_ref(Button::Pattern)), kTempoHold, ArgFrom::Value),
    bind(kGlobal, hold(button_ref(Button::Song)), kNavPeek, ArgFrom::Fixed, screen_arg(ScreenId::Song)),
    bind(kGlobal, hold_end(button_ref(Button::Song)), kNavPeekEnd, ArgFrom::Fixed, screen_arg(ScreenId::Song)),
    bind(kGlobal, tap(button_ref(Button::Performance)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Perform)),
    bind(kGlobal, tap(button_ref(Button::File)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::File)),
    bind(kGlobal, tap(button_ref(Button::Config)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Config)),
    // OS-NAV2: Shift + Pattern, tapped or held, opens the Navigator and it stays open (a tap at the press: Pattern has no Hold binding, so it does not wait).
    bind(kGlobal, tap(button_ref(Button::Pattern), kShift), kNavigatorOpen),
    // OS-HELP: Shift + Config opens the help page of the screen in view. Global, so every screen reaches it, Config included (a binding that names the modifier wins over the plain Config above it);
    // a modal overlay's context captures it, so under a dialog or the Navigator it does nothing. The app page (ModuleApp) captures too, so it binds the chord for itself, below.
    bind(kGlobal, tap(button_ref(Button::Config), kShift), kHelpOpen),
    // FX1 + FX2 are the two FX column buttons: Vol (20) and Fx (21) in the numbering of buttons.h (docs/design/product-ux.md
    // section 3, maintainer answer Q8). The chord opens the Learn overlay (and, in its own context, closes it).
    bind(kGlobal, chord(button_ref(Button::Vol), button_ref(Button::Fx)), kLearn),
    // Shift + Master: the next colour theme. PROVISIONAL until Config exists (OS-003b).
    bind(kGlobal, tap(button_ref(Button::Master), kShift), kThemeNext),
    bind(kGlobal, tap(button_ref(Button::Rec), kShift), kRecFree),
    // Play: play the pattern being edited, or stop (OS-005d). Global, so that the music can be stopped from any screen.
    bind(kGlobal, tap(button_ref(Button::Play)), kTransportPlay),
    // The arrows and the encoder move the cursor of whatever owns the input (the pattern grid, the song list: the Cursor
    // interface, screens/cursor.h). A screen with other ideas binds its own keys in its own context.
    bind(kGlobal, tap(button_ref(Button::Left)), kNavLeft),
    bind(kGlobal, tap(button_ref(Button::Right)), kNavRight),
    bind(kGlobal, tap(button_ref(Button::Up)), kNavUp),
    bind(kGlobal, tap(button_ref(Button::Down)), kNavDown),
    bind(kGlobal, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    // F1..F8: the soft keys of whatever screen is in view (its own labels). Global, not copied into each screen's context: a
    // screen that wants a key for itself binds it in its own context, which is consulted first.
    bind(kGlobal, tap(button_ref(Button::F1)), kSoftkey, ArgFrom::Fixed, 0),
    bind(kGlobal, tap(button_ref(Button::F2)), kSoftkey, ArgFrom::Fixed, 1),
    bind(kGlobal, tap(button_ref(Button::F3)), kSoftkey, ArgFrom::Fixed, 2),
    bind(kGlobal, tap(button_ref(Button::F4)), kSoftkey, ArgFrom::Fixed, 3),
    bind(kGlobal, tap(button_ref(Button::F5)), kSoftkey, ArgFrom::Fixed, 4),
    bind(kGlobal, tap(button_ref(Button::F6)), kSoftkey, ArgFrom::Fixed, 5),
    bind(kGlobal, tap(button_ref(Button::F7)), kSoftkey, ArgFrom::Fixed, 6),
    bind(kGlobal, tap(button_ref(Button::F8)), kSoftkey, ArgFrom::Fixed, 7),
    // OS-SHIFTROW rule 1: Shift + F1..F8 belong to the screen, never to Global. Global binds them to the null action (ui.none), so a Shift + F press the screen does not bind is nothing,
    // and never the plain key (core::resolve tries the modified press, in every context of the chain, before its no-modifier retry). A screen binds its own Shift + F keys in its context.
    bind(kGlobal, tap(button_ref(Button::F1), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F2), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F3), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F4), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F5), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F6), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F7), kShift), kUiNone),
    bind(kGlobal, tap(button_ref(Button::F8), kShift), kUiNone),

    // ---- Pattern: the screen's own keys, in every mode and field (OS-009 D3: a mode row overrides these, a field row specialises
    // them). The column buttons, Rec (tap: Browse and Edit), the choosers on F1..F3 (a tap toggles LENGTH and JUMP, a hold opens any until the release; F1 PATTERN is the hold only, OS-PATSWITCH: its tap opens nothing),
    // F5 and F7 (reserved: they say their label until their slice lands), F8 undo, the selection and the edit keys --------------------
    bind(kPattern, tap(button_ref(Button::Note)), kColumn, ArgFrom::Fixed, column_arg(Column::Note)),
    bind(kPattern, tap(button_ref(Button::Instr)), kColumn, ArgFrom::Fixed, column_arg(Column::Instr)),
    bind(kPattern, tap(button_ref(Button::Vol)), kColumn, ArgFrom::Fixed, column_arg(Column::Vol)),
    bind(kPattern, tap(button_ref(Button::Fx)), kFxColumn),
    // OS-FU3: a held Note, Instr, Vol (FX1: FX column 1) or Fx (FX2: column 2) button shows the step popup until it is let go (the StepPopup overlay's context binds the release).
    bind(kPattern, hold(button_ref(Button::Note)), kPopupOpen, ArgFrom::Fixed, 2),
    bind(kPattern, hold(button_ref(Button::Instr)), kPopupOpen, ArgFrom::Fixed, 0),
    bind(kPattern, hold(button_ref(Button::Fx)), kPopupOpen, ArgFrom::Fixed, 1),
    bind(kPattern, hold(button_ref(Button::Vol)), kPopupOpen, ArgFrom::Fixed, 3),
    bind(kPattern, tap(button_ref(Button::Rec)), kEditToggle),  // OS-RECSWAP: Browse <-> Edit, entered free; Shift + Rec (Global) enters it following
    // E-GLOB-06 (new Community OS behaviour): Rec, then Play inside the chord window (kGestureTimings.chord_ms), is Shift + Rec (LIVE, on and off). In Pattern only. The two taps then give this Chord and neither of
    // them: Rec's tap waits for the window (no stray Browse <-> Edit toggle), Play never waits (hint_wants in app.cpp, OS-BATCH-E2E: the transport start is a sound trigger and stays aligned with the pads),
    // so Play first is a plain Play tap.
    bind(kPattern, chord(button_ref(Button::Rec), button_ref(Button::Play)), kRecFree),
    bind(kPattern, tap(button_ref(Button::F1)), kChooserToggle, ArgFrom::Fixed, 0),
    bind(kPattern, tap(button_ref(Button::F2)), kChooserToggle, ArgFrom::Fixed, 1),
    bind(kPattern, tap(button_ref(Button::F3)), kChooserToggle, ArgFrom::Fixed, 2),
    bind(kPattern, hold(button_ref(Button::F1)), kChooserHold, ArgFrom::Fixed, 0),
    bind(kPattern, hold(button_ref(Button::F2)), kChooserHold, ArgFrom::Fixed, 1),
    bind(kPattern, hold(button_ref(Button::F3)), kChooserHold, ArgFrom::Fixed, 2),
    bind(kPattern, tap(button_ref(Button::F4)), kFillOpen),  // OS-PTOOLS: the Fill dialog
    bind(kPattern, tap(button_ref(Button::F5)), kPreview),
    bind(kPattern, tap(button_ref(Button::F6)), kPatternInvert),  // OS-CFGWIRE: INVERT (edit mode, a block)
    // F8 is the UNDO soft key of Pattern and Song (OS-002c), Shift + F8 its redo. The overlays' own F8 (Back) comes first in
    // their capturing contexts; with no document attached the key says "nothing to undo".
    bind(kPattern, tap(button_ref(Button::F7)), kToolsOpen),  // OS-H3A: the TOOLS list (EXPORT joins it with OS-WAVEXP)
    bind(kPattern, tap(button_ref(Button::F8)), kUndo),
    bind(kPattern, tap(button_ref(Button::F8), kShift), kRedo),
    // OS-PADS-A / OS-PADOCT: Shift + F1 cycles the pads' octave, Shift + F2..F7 choose the pads' mode (the argument is the mode's number, pad_layer.h kPadModeKey); these also shadow Global's quarter jumps on Pattern.
    // OS-STEPPADOFF: the STEP mode has no key: the mode is silent by design until OS-STEPPAD redesigns it.
    bind(kPattern, tap(button_ref(Button::F1), kShift), kPadsOctave),
    bind(kPattern, tap(button_ref(Button::F2), kShift), kPadsMode, ArgFrom::Fixed, 0),
    bind(kPattern, tap(button_ref(Button::F3), kShift), kPadsMode, ArgFrom::Fixed, 1),
    bind(kPattern, tap(button_ref(Button::F4), kShift), kPadsMode, ArgFrom::Fixed, 3),
    bind(kPattern, tap(button_ref(Button::F5), kShift), kPadsMode, ArgFrom::Fixed, 2),
    bind(kPattern, tap(button_ref(Button::F6), kShift), kPadsMode, ArgFrom::Fixed, 5),
    bind(kPattern, tap(button_ref(Button::F7), kShift), kPadsMode, ArgFrom::Fixed, 6),
    // OS-QJUMP: Shift + Note, Instr, Vol (FX1) and Fx (FX2) jump to 0, 1/4, 2/4, 3/4 of the pattern (Song and File have theirs on Shift + F1..F4). Shift + Fx was FILL again (OS-PTOOLS); the F4 dialog is the Fill key.
    bind(kPattern, tap(button_ref(Button::Note), kShift), kJumpFraction, ArgFrom::Fixed, 0),
    bind(kPattern, tap(button_ref(Button::Instr), kShift), kJumpFraction, ArgFrom::Fixed, 1),
    bind(kPattern, tap(button_ref(Button::Vol), kShift), kJumpFraction, ArgFrom::Fixed, 2),
    bind(kPattern, tap(button_ref(Button::Fx), kShift), kJumpFraction, ArgFrom::Fixed, 3),
    // OS-ZOOM: Note is held to modify: Note + Instr cycles the zoom (5, 8, 12 tracks), Note + Left / Right moves the cursor a whole track. Any other button pressed with Note cancels its tap and hold (Note + Vol and Note + Fx are Vol and Fx).
    bind(kPattern, tap(button_ref(Button::Instr), kNoteKey), kPatternZoom),
    bind(kPattern, tap(button_ref(Button::Left), kNoteKey), kTrack, ArgFrom::Fixed, -1),
    bind(kPattern, tap(button_ref(Button::Right), kNoteKey), kTrack, ArgFrom::Fixed, 1),
    bind(kPattern, tap(button_ref(Button::Play), kShift), kTransportPlayCursor),  // OS-PLAYCUR: play from the cursor's row (a block selected: the Focus Loop's key, the same until it exists)
    // The block: Shift + the arrows extend it from where Shift was first pressed (Shift + Up at step 0: the column, then every track).
    // The argument is 10 * dx + dy.
    bind(kPattern, tap(button_ref(Button::Left), kShift), kSelectExtend, ArgFrom::Fixed, -10),
    bind(kPattern, tap(button_ref(Button::Right), kShift), kSelectExtend, ArgFrom::Fixed, 10),
    bind(kPattern, tap(button_ref(Button::Up), kShift), kSelectExtend, ArgFrom::Fixed, -1),
    bind(kPattern, tap(button_ref(Button::Down), kShift), kSelectExtend, ArgFrom::Fixed, 1),
    // Insert, Delete and their Shift variants: insert a step, clear (the field says what), HOME (collapse and go to step 0), pull (the inverse of Insert).
    bind(kPattern, tap(button_ref(Button::Insert)), kStepInsert),
    bind(kPattern, tap(button_ref(Button::Insert), kShift), kJumpFraction, ArgFrom::Fixed, 0),
    bind(kPattern, tap(button_ref(Button::Delete)), kStepClear, ArgFrom::Fixed, 0),
    bind(kPattern, tap(button_ref(Button::Delete), kShift), kStepPull),
    // OS-PATCOPY: Copy takes the block (or the step), Shift + Copy writes it at the cursor, as Song and Perform do with theirs.
    bind(kPattern, tap(button_ref(Button::Copy)), kBlockCopy),
    bind(kPattern, tap(button_ref(Button::Copy), kShift), kBlockPaste),

    // ---- OS-PADS-B: the pads of a fixed PIANO / ISO / STEP / PATS / VU mode (the context is in the chain only then: pad_layer.h); one binding, the action acts by the mode ---
    bind(kPadsModeCtx, tap(pad_ref(core::kAnyId)), kPadsPlay, ArgFrom::InputId, 0),

    // ---- Pattern / Browse (the default mode): the pads preview, the encoder scrolls the steps; this mode's rows win over the field's ---
    bind(kBrowse, tap(pad_ref(core::kAnyId)), kPadPreview, ArgFrom::InputId, 0),
    bind(kBrowse, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kBrowse, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // a page of steps a detent (OS-SHIFTENC); and a Shift binding of the field would win over a plain one here
    bind(kBrowse, hold_encoder(encoder_ref(0), button_ref(Button::Fx)), kNavScroll, ArgFrom::Value),  // Fx held + the encoder scrolls here, it must not reach the Fx field's type edit
    // (Pattern / Follow had the Browse rows until OS-004d retired the mode: Browse follows the playhead too. Its context stays named, with no bindings.)

    // (Pattern / Select, the block mark whose arrows extended the block, went with the mode: OS-LIVEREC. Its context stays named, with no bindings. The block is Shift + the arrows, in every mode.)

    // ---- Pattern fields: what the pads and the encoder do depends on the column under the cursor (Edit; Browse's rows come
    // first). A pad enters a note by the pad board (also in the Instr column: OS-PADHOLD), a volume or an FX value; the encoder changes it, and the Instr column's instrument is the encoder's or the hold popup's. ----------
    bind(kNote, tap(pad_ref(core::kAnyId)), kStepNotePad, ArgFrom::InputId, 0),
    bind(kNote, tap(encoder_ref(0)), kStepNoteDelta, ArgFrom::Value),
    bind(kNote, tap(encoder_ref(0), kShift), kStepNoteOctave, ArgFrom::Value),
    bind(kInstr, tap(pad_ref(core::kAnyId)), kStepNotePad, ArgFrom::InputId, 0),  // OS-PADHOLD: a pad in the Instr column writes the note, as in the Note column; the instrument is the hold popup's or the encoder's
    bind(kInstr, tap(encoder_ref(0)), kStepInstrumentDelta, ArgFrom::Value),
    bind(kInstr, tap(encoder_ref(0), kShift), kStepInstrumentCoarse, ArgFrom::Value),  // OS-SHIFTENC: x8
    bind(kVol, tap(pad_ref(core::kAnyId)), kStepVolPad, ArgFrom::InputId, 0),
    bind(kVol, tap(encoder_ref(0)), kStepVolDelta, ArgFrom::Value),
    bind(kVol, tap(encoder_ref(0), kShift), kStepVolCoarse, ArgFrom::Value),  // OS-SHIFTENC: x10
    bind(kFx, tap(pad_ref(core::kAnyId)), kStepFxPad, ArgFrom::InputId, 0),
    bind(kFx, tap(encoder_ref(0)), kStepFxValueDelta, ArgFrom::Value),
    bind(kFx, tap(encoder_ref(0), kShift), kStepFxValueCoarse, ArgFrom::Value),  // OS-SHIFTENC: x16, one nibble
    bind(kFx, hold_encoder(encoder_ref(0), button_ref(Button::Fx)), kStepFxTypeDelta, ArgFrom::Value),

    // ---- Song (OS-SONG1): the arrows (Global's) move its list and Shift + Up / Down select a range of slots. F1 PLAY PAT, F2 PLAY SNG (the Play button's own action, which on this screen
    // plays the song from the selected row: OS-005e), F3 ADD SLOT (Shift + F3: with a copy of the pattern as a new pattern), F4 DELETE SLOT, F5 / F6 the
    // slot's pattern - / +, F7 TEMPO (the encoder on the tempo field; each press taps the tempo), F8 UNDO and Shift + F8 REDO as on Pattern. Insert and Delete are the slot keys too, the
    // encoder is the pattern of the slot (the tempo after F7; Shift: a tenth of a BPM), Copy copies the range (or the slot) and Shift + Copy inserts it at the cursor ----------------------
    bind(kSong, tap(button_ref(Button::F1)), kSongPlayPattern),
    bind(kSong, tap(button_ref(Button::F2)), kTransportPlay),
    bind(kSong, tap(button_ref(Button::F3)), kSongAdd, ArgFrom::Fixed, 0),
    bind(kSong, tap(button_ref(Button::F4)), kSongDelete),
    // Q1 of docs/specs/SONG-UX.md (maintainer's to confirm; the OG's own Song F5 / F6 are an open research question): F5 UNIQUE and F6 TOOLS take the keys that were the slot's pattern - / +, which stay on the encoder.
    // Reversing it is these two lines and kSongSoftKeys (song_screen.h): song.pattern_delta stays registered.
    bind(kSong, tap(button_ref(Button::F5)), kSongUnique),
    bind(kSong, tap(button_ref(Button::F6)), kSongTools),
    bind(kSong, tap(button_ref(Button::F7)), kSongTempo),
    bind(kSong, tap(button_ref(Button::F8)), kUndo),
    bind(kSong, tap(button_ref(Button::F8), kShift), kRedo),
    bind(kSong, tap(button_ref(Button::Insert)), kSongAdd, ArgFrom::Fixed, 0),
    bind(kSong, tap(button_ref(Button::Insert), kShift), kSongAdd, ArgFrom::Fixed, 1),  // OS-SHIFTROW: DUP PAT (the slot with a copy of the pattern), moved from Shift + F3
    bind(kSong, tap(button_ref(Button::F1), kShift), kJumpFraction, ArgFrom::Fixed, 0),  // OS-SHIFTROW: the quarter jumps are Song's own
    bind(kSong, tap(button_ref(Button::F2), kShift), kJumpFraction, ArgFrom::Fixed, 1),
    bind(kSong, tap(button_ref(Button::F3), kShift), kJumpFraction, ArgFrom::Fixed, 2),
    bind(kSong, tap(button_ref(Button::F4), kShift), kJumpFraction, ArgFrom::Fixed, 3),
    bind(kSong, tap(button_ref(Button::F5), kShift), kJumpEnd),
    bind(kSong, tap(button_ref(Button::F6), kShift), kSongName),  // OS-NAMES: NAME, the pattern of the slot
    bind(kSong, tap(button_ref(Button::F7), kShift), kSongPads),  // OS-SONGPADS: PADS, the pads switch between TRACKS and SLOTS
    bind(kSong, tap(button_ref(Button::Delete)), kSongClear),  // OS-SONGMAP: the cells of the map's rectangle in the map, else the slot (F4's action)
    // OS-SONGSHARE S4 (Q2a): Note jumps to the next slot that holds the cursor's pattern, Shift + Note to the previous one; both wrap. Note is a plain key on Song (no hold, no held-modifier rows), so it taps at the press.
    bind(kSong, tap(button_ref(Button::Note)), kSongNextUse, ArgFrom::Fixed, 1),
    bind(kSong, tap(button_ref(Button::Note), kShift), kSongNextUse, ArgFrom::Fixed, -1),
    bind(kSong, tap(button_ref(Button::Delete), kShift), kSongCut),  // OS-SONGSHARE S2: CUT, the slots (PAT) or the cells (the map) into their clipboard and out; it fell back to the plain Delete before
    bind(kSong, tap(encoder_ref(0)), kSongTurn, ArgFrom::Value),
    bind(kSong, tap(encoder_ref(0), kShift), kSongTurnFine, ArgFrom::Value),
    bind(kSong, tap(button_ref(Button::Up), kShift), kSongSelect, ArgFrom::Fixed, -1),
    bind(kSong, tap(button_ref(Button::Down), kShift), kSongSelect, ArgFrom::Fixed, 1),
    bind(kSong, tap(button_ref(Button::Copy)), kSongCopy),
    // OS-SONGSHARE S3: Copy held + Up / Down moves the range of slots one position (Q1a). Copy is a held modifier here, so Copy and Shift + Copy fire on the release; Pattern and Perform keep their Copy at the press. Shift + Up / Down stay the range.
    bind(kSong, tap(button_ref(Button::Up), kCopyKey), kSongMove, ArgFrom::Fixed, -1),
    bind(kSong, tap(button_ref(Button::Down), kCopyKey), kSongMove, ArgFrom::Fixed, 1),
    bind(kSong, tap(button_ref(Button::Copy), kShift), kSongPaste),
    bind(kSong, tap(button_ref(Button::Left), kShift), kSongSelectTracks, ArgFrom::Fixed, -1),  // OS-SONGMAP: the range of tracks, in the map
    bind(kSong, tap(button_ref(Button::Right), kShift), kSongSelectTracks, ArgFrom::Fixed, 1),
    // OS-SONGMAP: while the question of 1.6 is up (the screen's field level) F1 answers ALL and F2 UNIQUE; every other key falls through to the screen's own and takes the question back (App::run_once).
    bind(kSongAskCtx, tap(button_ref(Button::F1)), kSongAnswer, ArgFrom::Fixed, 0),
    bind(kSongAskCtx, tap(button_ref(Button::F2)), kSongAnswer, ArgFrom::Fixed, 1),
    // The TOOLS dialog (captures): Up / Down choose a row, Left / Right or the encoder change its value (Shift + encoder: the row's coarse step), F1 APPLY (one undo unit), F8 BACK.
    bind(kSongToolsCtx, tap(button_ref(Button::Up)), kSongToolsMove, ArgFrom::Fixed, -1),
    bind(kSongToolsCtx, tap(button_ref(Button::Down)), kSongToolsMove, ArgFrom::Fixed, 1),
    bind(kSongToolsCtx, tap(button_ref(Button::Left)), kSongToolsDelta, ArgFrom::Fixed, -1),
    bind(kSongToolsCtx, tap(button_ref(Button::Right)), kSongToolsDelta, ArgFrom::Fixed, 1),
    bind(kSongToolsCtx, tap(encoder_ref(0)), kSongToolsDelta, ArgFrom::Value),
    bind(kSongToolsCtx, tap(encoder_ref(0), kShift), kSongToolsCoarse, ArgFrom::Value),
    bind(kSongToolsCtx, tap(button_ref(Button::F1)), kSongToolsApply),
    bind(kSongToolsCtx, tap(button_ref(Button::F8)), kBack),
    // Rec, while the song plays: the cursor's slot is next, at the end of the pattern now playing (the slot blinks while it waits; Rec again on it cancels). Rec has no other use on Song
    // (the Rec button does nothing there), so it is free here for a live-safe key that a stray arrow press cannot trigger.
    bind(kSong, tap(button_ref(Button::Rec)), kSongQueueJump),
    // OS-SONGPADS: the pads. In TRACKS mode (and for a lone Shift hold) the innermost PadsTracks layer takes them; this row is reached in SLOTS mode only (song.pad checks the mode).
    bind(kSong, tap(pad_ref(core::kAnyId)), kSongPad, ArgFrom::InputId, 0),

    // ---- Help (OS-HELP; captures): Up / Down and the encoder scroll a line, Shift + Up / Down and Shift + the encoder a screenful, Left / Right turn the pages (Shift + Left / Right the same: the fall-back to the plain
    // key). F8 closes it by name; every other button and every pad closes it too, without acting (App::dispatch_gestures: a press this context captures, Shift apart).
    bind(kHelpCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kHelpCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kHelpCtx, tap(button_ref(Button::Left)), kNavLeft),
    bind(kHelpCtx, tap(button_ref(Button::Right)), kNavRight),
    bind(kHelpCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kHelpCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),
    bind(kHelpCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kHelpCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),
    bind(kHelpCtx, tap(button_ref(Button::F8)), kBack),

    // ---- File (OS-010a): F1 OPEN the project under the cursor, F2 RESCAN the folder. The arrows and the encoder are Global's (the Cursor: Up / Down move,
    // Left / Right jump by first letter), the quarter jumps likewise -------------------------------------------------------------------
    bind(kFileCtx, tap(button_ref(Button::F1)), kFileOpen),
    bind(kFileCtx, tap(button_ref(Button::F2)), kFileRescan),
    bind(kFileCtx, tap(button_ref(Button::F3)), kFileNew),
    // F3 NEW (OS-007o: the platform's document reset; a second press when there are unsaved changes), OS-007n: F4 SAVE the project into its folder, F5 SAVE AS a new folder
    bind(kFileCtx, tap(button_ref(Button::F4)), kFileSave),
    bind(kFileCtx, tap(button_ref(Button::F5)), kFileSaveAs),
    // OS-FILES part A: F6 the IMPORT page (module files), F7 the engine the import writes for, F8 the last import's report
    bind(kFileCtx, tap(button_ref(Button::F6)), kFilePage),
    bind(kFileCtx, tap(button_ref(Button::F7)), kFileEngine),
    bind(kFileCtx, tap(button_ref(Button::F8)), kFileReport),
    // OS-FILES part B (PROJECTS page; each acts by the page, and says nothing on the others): the Delete key marks the folder under the cursor deleted (UI map rule 6: Delete clears the thing under the cursor), Shift + F7 PURGEs
    bind(kFileCtx, tap(button_ref(Button::Delete)), kFileDelete),
    bind(kFileCtx, tap(button_ref(Button::F7), kShift), kFilePurge),
    // OS-WAVEXP: the EXPORT page (F7 of the PROJECTS page; F6 and F7 stay the File context's, file.page and file.engine act by the page): F1 starts the row, the encoder picks the pattern of the PATTERN row,
    // F2, F3 and F8 are blank (F4 SAVE and F5 SAVE AS are the screen's). The progress overlay's one key is F8 CANCEL; its context captures, so nothing else gets through while the export runs.
    bind(kFileExportCtx, tap(button_ref(Button::F1)), kFileExport),
    bind(kFileExportCtx, tap(button_ref(Button::F2)), kUiNone),
    bind(kFileExportCtx, tap(button_ref(Button::F3)), kUiNone),
    bind(kFileExportCtx, tap(button_ref(Button::F8)), kUiNone),
    bind(kFileExportCtx, tap(button_ref(Button::F7), kShift), kUiNone),  // OS-FILES part B: PURGE is the PROJECTS page's
    bind(kFileExportCtx, tap(encoder_ref(0)), kExportValue, ArgFrom::Value),
    bind(kFileExportCtx, tap(encoder_ref(0), kShift), kExportCoarse, ArgFrom::Value),  // OS-SHIFTENC
    bind(kExportProgressCtx, tap(button_ref(Button::F8)), kExportCancel),
    // OS-SHIFTROW: the quarter jumps are the list screens' own
    bind(kFileCtx, tap(button_ref(Button::F1), kShift), kJumpFraction, ArgFrom::Fixed, 0),
    bind(kFileCtx, tap(button_ref(Button::F2), kShift), kJumpFraction, ArgFrom::Fixed, 1),
    bind(kFileCtx, tap(button_ref(Button::F3), kShift), kJumpFraction, ArgFrom::Fixed, 2),
    bind(kFileCtx, tap(button_ref(Button::F4), kShift), kJumpFraction, ArgFrom::Fixed, 3),
    bind(kFileCtx, tap(button_ref(Button::F5), kShift), kJumpEnd),
    bind(kFileCtx, tap(button_ref(Button::F6), kShift), kFileName),  // OS-NAMES: NAME, the project's title
    // OS-NAV3: Shift + Up / Down a page (the rows the list shows), PROJECTS and IMPORT alike
    bind(kFileCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),
    bind(kFileCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kFileCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC (the EXPORT page's own is export.coarse)

    // ---- Sample load (OS-011a): F1 LOAD the WAV under the cursor into the target instrument, F2 UP, F3 ENTER the folder, F4 ADD into the first free instrument,
    // F5 PLAY (TODO), F6 RESCAN; a pad chooses the target instrument as on PARAMS. The arrows and the encoder are Global's (the Cursor: Left / Right jump by letter) ---------
    bind(kSampleLoadCtx, tap(button_ref(Button::F1)), kSampleLoad, ArgFrom::Fixed, 0),
    bind(kSampleLoadCtx, tap(button_ref(Button::F2)), kSampleUp),
    bind(kSampleLoadCtx, tap(button_ref(Button::F3)), kSampleEnter),
    bind(kSampleLoadCtx, tap(button_ref(Button::F4)), kSampleLoad, ArgFrom::Fixed, 1),
    bind(kSampleLoadCtx, tap(button_ref(Button::F5)), kSamplePlay),
    bind(kSampleLoadCtx, tap(button_ref(Button::F6)), kSampleRescan),
    bind(kSampleLoadCtx, tap(button_ref(Button::F7)), kSampleWt),  // OS-GRAN: WT AUTO / LEN / 256 .. 2048 / OFF
    bind(kSampleLoadCtx, tap(button_ref(Button::F8)), kSampleChannel),  // OS-LOADCH: CH MIX / LEFT / RIGHT, for a stereo file
    // OS-AUDIT-40 (E-LOAD-11, E-LOAD-24, new Community OS behaviour): a pad chooses the target instrument and plays it (the Pattern pads' path; a sound loaded a moment ago is heard after the handover, SOUNDS WHEN STOPPED)
    bind(kSampleLoadCtx, tap(pad_ref(core::kAnyId)), kPadAudition, ArgFrom::InputId, 0),
    // OS-AUDIT-26 (E-GLOB-07, new Community OS behaviour): the screens with no pad job play the current instrument
    bind(kMixerCtx, tap(pad_ref(core::kAnyId)), kPadAudition, ArgFrom::InputId, 0),
    bind(kConfigCtx, tap(pad_ref(core::kAnyId)), kPadAudition, ArgFrom::InputId, 0),
    bind(kFileCtx, tap(pad_ref(core::kAnyId)), kPadAudition, ArgFrom::InputId, 0),
    bind(kSampleLoadCtx, tap(button_ref(Button::F1), kShift), kJumpFraction, ArgFrom::Fixed, 0),
    bind(kSampleLoadCtx, tap(button_ref(Button::F2), kShift), kJumpFraction, ArgFrom::Fixed, 1),
    bind(kSampleLoadCtx, tap(button_ref(Button::F3), kShift), kJumpFraction, ArgFrom::Fixed, 2),
    bind(kSampleLoadCtx, tap(button_ref(Button::F4), kShift), kJumpFraction, ArgFrom::Fixed, 3),
    bind(kSampleLoadCtx, tap(button_ref(Button::F5), kShift), kJumpEnd),
    bind(kSampleLoadCtx, tap(button_ref(Button::F7), kShift), kSampleTarget, ArgFrom::Fixed, -1),  // OS-SLDTARGET: INST - / INST +, the target instrument (a pad picks it too)
    bind(kSampleLoadCtx, tap(button_ref(Button::F8), kShift), kSampleTarget, ArgFrom::Fixed, 1),
    // OS-AUDIT-A7 (SLD-MULTI, E-LOAD-14, new Community OS behaviour): Shift + Right marks the file (or takes its mark back) and steps down, Shift + Left clears the marks; F1 / F4 then load the marked files
    bind(kSampleLoadCtx, tap(button_ref(Button::Right), kShift), kSampleMark, ArgFrom::Fixed, 0),
    bind(kSampleLoadCtx, tap(button_ref(Button::Left), kShift), kSampleMark, ArgFrom::Fixed, 1),
    bind(kSampleLoadCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),  // OS-NAV3
    bind(kSampleLoadCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kSampleLoadCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC

    // ---- Wave / Chop (OS-011a). The screen's keys, on both pages: the encoder moves the selected point or slice (Shift: 16 columns a click), a pad plays slice n and
    // selects it (Shift + pad: silently), F8 UNDO and Shift + F8 REDO as everywhere. Left / Right (the point or the slice) and Up / Down (zoom) are Global's (the Cursor).
    // WAVE page: F1..F4 pick START, LOOP S, LOOP E, END; F5 ZC; F6 PREVIEW, F7 zoom out. The CHOP page's field context comes first and takes F1..F7 for itself ----------------
    bind(kWaveCtx, tap(encoder_ref(0)), kWaveNudge, ArgFrom::Value),
    bind(kWaveCtx, tap(encoder_ref(0), kShift), kWaveNudgeCoarse, ArgFrom::Value),
    bind(kWaveCtx, tap(pad_ref(core::kAnyId)), kWavePad, ArgFrom::InputId, 0),
    bind(kWaveCtx, tap(pad_ref(core::kAnyId), kShift), kWavePadSelect, ArgFrom::InputId, 0),
    bind(kWaveCtx, tap(button_ref(Button::F1)), kWavePoint, ArgFrom::Fixed, 0),
    bind(kWaveCtx, tap(button_ref(Button::F2)), kWavePoint, ArgFrom::Fixed, 1),
    bind(kWaveCtx, tap(button_ref(Button::F3)), kWavePoint, ArgFrom::Fixed, 2),
    bind(kWaveCtx, tap(button_ref(Button::F4)), kWavePoint, ArgFrom::Fixed, 3),
    bind(kWaveCtx, tap(button_ref(Button::F5)), kWaveZc),
    // OS-FU3: F6 is the held PREVIEW (the zoom in is Up's), Shift + F6 plays from where the screen is
    bind(kWaveCtx, tap(button_ref(Button::F6)), kPreviewHold, ArgFrom::Fixed, static_cast<int>(Button::F6)),
    bind(kWaveCtx, tap(button_ref(Button::F6), kShift), kPreviewHold, ArgFrom::Fixed, static_cast<int>(Button::F6) | 0x100),
    bind(kWaveCtx, tap(button_ref(Button::F7)), kWaveZoom, ArgFrom::Fixed, -1),
    bind(kWaveCtx, tap(button_ref(Button::F8)), kUndo),
    bind(kWaveCtx, tap(button_ref(Button::F8), kShift), kRedo),
    // OS-INSTPICK: a held Instr shows the instrument list (Params' popup) and the choice is the instrument in view; only the samples can be chosen
    bind(kWaveCtx, hold(button_ref(Button::Instr)), kPopupOpen, ArgFrom::Fixed, 0),
    // CHOP page: F1 AUTO, F2 EVEN, F3 ZC, F4 ADD, F5 DEL, F6 PREVIEW, F7 zoom out; Shift + Up / Down: how many slices EVEN makes
    bind(kChopCtx, tap(button_ref(Button::F1)), kWaveAuto),
    bind(kChopCtx, tap(button_ref(Button::F2)), kWaveEven),
    bind(kChopCtx, tap(button_ref(Button::F3)), kWaveZc),
    bind(kChopCtx, tap(button_ref(Button::F4)), kWaveAdd),
    bind(kChopCtx, tap(button_ref(Button::F5)), kWaveDel),
    bind(kChopCtx, tap(button_ref(Button::Up), kShift), kWaveEvenN, ArgFrom::Fixed, 1),
    bind(kChopCtx, tap(button_ref(Button::Down), kShift), kWaveEvenN, ArgFrom::Fixed, -1),
    // OS-INSTUI S4: Shift + F1 MODE on every view: the list of the eight play modes; the pick is play_mode_set, as the source page's PLAY MODE row.
    bind(kWaveCtx, tap(button_ref(Button::F1), kShift), kWaveMode),
    bind(kPlayModePickerCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kPlayModePickerCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kPlayModePickerCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kPlayModePickerCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC: a page a detent
    bind(kPlayModePickerCtx, tap(button_ref(Button::F1)), kWaveModePick),
    bind(kPlayModePickerCtx, tap(button_ref(Button::F8)), kBack),
    // WAVETABLE view (Preview, Position, Window): F1 POSITION, F2 WINDOW pick the row (Left / Right too), the encoder edits it as the source page's rows (Shift: coarse, Delete: default);
    // F3..F5 have nothing here. GRANULAR view (Position, Length, Shape, Loop): F1..F4 the row, F5 nothing. PREVIEW (F6), ZOOM- (F7), UNDO and the pads are the screen's.
    bind(kWtCtx, tap(encoder_ref(0)), kParamEdit, ArgFrom::Value),
    bind(kWtCtx, tap(encoder_ref(0), kShift), kParamCoarse, ArgFrom::Value),
    bind(kWtCtx, tap(button_ref(Button::Delete)), kParamReset),
    bind(kWtCtx, tap(button_ref(Button::F1)), kWavePoint, ArgFrom::Fixed, 0),
    bind(kWtCtx, tap(button_ref(Button::F2)), kWavePoint, ArgFrom::Fixed, 1),
    bind(kWtCtx, tap(button_ref(Button::F3)), kUiNone),
    bind(kWtCtx, tap(button_ref(Button::F4)), kUiNone),
    bind(kWtCtx, tap(button_ref(Button::F5)), kUiNone),
    bind(kGranCtx, tap(encoder_ref(0)), kParamEdit, ArgFrom::Value),
    bind(kGranCtx, tap(encoder_ref(0), kShift), kParamCoarse, ArgFrom::Value),
    bind(kGranCtx, tap(button_ref(Button::Delete)), kParamReset),
    bind(kGranCtx, tap(button_ref(Button::F1)), kWavePoint, ArgFrom::Fixed, 0),
    bind(kGranCtx, tap(button_ref(Button::F2)), kWavePoint, ArgFrom::Fixed, 1),
    bind(kGranCtx, tap(button_ref(Button::F3)), kWavePoint, ArgFrom::Fixed, 2),
    bind(kGranCtx, tap(button_ref(Button::F4)), kWavePoint, ArgFrom::Fixed, 3),
    bind(kGranCtx, tap(button_ref(Button::F5)), kUiNone),

    // ---- Sample edit (OS-011a): Left / Right pick the selection's edge and Up / Down zoom (the Cursor, Global's); the encoder moves the edge (Shift: 16 columns); F1..F6 the
    // effects (arg = EditOp: crop, normalise, reverse, fade in, fade out, amplify), F7 ALL clears the selection, F8 / Shift + F8 the sample's own undo and redo, Shift + Up / Down
    // Amplify's percentage; a pad chooses the instrument ---------------------------------------------------------------------------------------------------------------
    bind(kSeditCtx, tap(encoder_ref(0)), kSeditMove, ArgFrom::Value),
    bind(kSeditCtx, tap(encoder_ref(0), kShift), kSeditMoveCoarse, ArgFrom::Value),
    bind(kSeditCtx, tap(pad_ref(core::kAnyId)), kInstrument, ArgFrom::InputId, 1),
    bind(kSeditCtx, hold(button_ref(Button::Instr)), kPopupOpen, ArgFrom::Fixed, 0),  // OS-INSTPICK: as on Wave, the held Instr's list of the samples
    bind(kSeditCtx, tap(button_ref(Button::F1)), kSeditRun, ArgFrom::Fixed, 0),
    bind(kSeditCtx, tap(button_ref(Button::F2)), kSeditRun, ArgFrom::Fixed, 1),
    bind(kSeditCtx, tap(button_ref(Button::F3)), kSeditRun, ArgFrom::Fixed, 2),
    bind(kSeditCtx, tap(button_ref(Button::F4)), kSeditRun, ArgFrom::Fixed, 3),
    bind(kSeditCtx, tap(button_ref(Button::F5)), kSeditRun, ArgFrom::Fixed, 4),
    bind(kSeditCtx, tap(button_ref(Button::F6)), kSeditRun, ArgFrom::Fixed, 5),
    // OS-FU3: F7 is the held PREVIEW (Shift + F7 from the selection's edge); ALL moved to Delete, the save as a WAV to Shift + F6 (OS-007n: the key row is full)
    bind(kSeditCtx, tap(button_ref(Button::F7)), kPreviewHold, ArgFrom::Fixed, static_cast<int>(Button::F7)),
    bind(kSeditCtx, tap(button_ref(Button::F7), kShift), kPreviewHold, ArgFrom::Fixed, static_cast<int>(Button::F7) | 0x100),
    bind(kSeditCtx, tap(button_ref(Button::Delete)), kSeditAll),
    bind(kSeditCtx, tap(button_ref(Button::F6), kShift), kSeditSaveWav),
    bind(kSeditCtx, tap(button_ref(Button::F1), kShift), kSeditAmiga, ArgFrom::Fixed, 0),
    bind(kSeditCtx, tap(button_ref(Button::F2), kShift), kSeditAmiga, ArgFrom::Fixed, 1),
    bind(kSeditCtx, tap(button_ref(Button::F3), kShift), kSeditQ8),
    bind(kSeditCtx, tap(button_ref(Button::F4), kShift), kSeditRun, ArgFrom::Fixed, 7),  // OS-SEDFX: EditOp::DcRemove (sample_dsp.h)
    bind(kSeditCtx, tap(button_ref(Button::F5), kShift), kSeditInstrument, ArgFrom::Fixed, 1),  // OS-QAFIX-27: NEXT INS, the instrument being edited (a pad or the held Instrument picks it too)
    bind(kSeditCtx, tap(button_ref(Button::F8)), kSeditUndo),
    bind(kSeditCtx, tap(button_ref(Button::F8), kShift), kSeditRedo),
    // OS-AUDIT-15 (new Community OS behaviour): the FX page (the Sample edit button again), the screen's field level: F1 FILTER, F2 COMPRESSOR, the encoder changes the row; F3..F6 do nothing there
    // (the plain keys of the edit page, REVERSE .. AMP, must not act behind a blank label); F7 PREVIEW, F8 UNDO and every Shift key are the screen's.
    bind(kSeditFxCtx, tap(encoder_ref(0)), kSeditFx, ArgFrom::Value),
    bind(kSeditFxCtx, tap(encoder_ref(0), kShift), kSeditFxCoarse, ArgFrom::Value),
    bind(kSeditFxCtx, tap(button_ref(Button::F1)), kSeditRun, ArgFrom::Fixed, static_cast<int>(EditOp::Filter)),
    bind(kSeditFxCtx, tap(button_ref(Button::F2)), kSeditRun, ArgFrom::Fixed, static_cast<int>(EditOp::Compressor)),
    bind(kSeditFxCtx, tap(button_ref(Button::F3)), kUiNone),
    bind(kSeditFxCtx, tap(button_ref(Button::F4)), kUiNone),
    bind(kSeditFxCtx, tap(button_ref(Button::F5)), kUiNone),
    bind(kSeditFxCtx, tap(button_ref(Button::F6)), kUiNone),
    bind(kSeditFxCtx, tap(button_ref(Button::Up), kShift), kUiNone),  // the AMP row of the edit page is not on this page
    bind(kSeditFxCtx, tap(button_ref(Button::Down), kShift), kUiNone),
    bind(kSeditCtx, tap(button_ref(Button::Up), kShift), kSeditAmp, ArgFrom::Fixed, 1),
    bind(kSeditCtx, tap(button_ref(Button::Down), kShift), kSeditAmp, ArgFrom::Fixed, -1),

    // ---- Config (OS-011a): the device settings pages generated from the descriptors, as PARAMS: the encoder edits the row (Shift: the coarse step), Delete puts it back to its default,
    // F1..F6 are the six pages (MIDI, DISPLAY, SD, INFO, USB, PATTERN), F7 is APPLY (a row that re-enumerates USB only holds the turn until then). Left / Right edit the row (OS-CFG-ARROWS), Up / Down are Global's (the Cursor) ---------------
    bind(kConfigCtx, tap(encoder_ref(0)), kParamEdit, ArgFrom::Value),
    bind(kConfigCtx, tap(encoder_ref(0), kShift), kParamCoarse, ArgFrom::Value),
    // OS-CFG-ARROWS (new Community OS behaviour, replaces OS-SHIFTROW rule 5 here): Left / Right are the encoder's detent on the row (an auto-repeat is one tap per detent), Shift + Left / Right its coarse
    // step; the page changes only with F1..F6 or the screen button (the Instrument's pattern, OS-INSTUI-ARROWS).
    bind(kConfigCtx, tap(button_ref(Button::Left)), kParamEdit, ArgFrom::Fixed, -1),
    bind(kConfigCtx, tap(button_ref(Button::Right)), kParamEdit, ArgFrom::Fixed, 1),
    bind(kConfigCtx, tap(button_ref(Button::Left), kShift), kParamCoarse, ArgFrom::Fixed, -1),
    bind(kConfigCtx, tap(button_ref(Button::Right), kShift), kParamCoarse, ArgFrom::Fixed, 1),
    bind(kConfigCtx, tap(button_ref(Button::Delete)), kParamReset),
    bind(kConfigCtx, tap(button_ref(Button::F1)), kParamGroup, ArgFrom::Fixed, 0),
    bind(kConfigCtx, tap(button_ref(Button::F2)), kParamGroup, ArgFrom::Fixed, 1),
    bind(kConfigCtx, tap(button_ref(Button::F3)), kParamGroup, ArgFrom::Fixed, 2),
    bind(kConfigCtx, tap(button_ref(Button::F4)), kParamGroup, ArgFrom::Fixed, 3),
    bind(kConfigCtx, tap(button_ref(Button::F5)), kParamGroup, ArgFrom::Fixed, 4),  // OS-USBRET
    bind(kConfigCtx, tap(button_ref(Button::F6)), kParamGroup, ArgFrom::Fixed, 5),  // OS-LIVEREC
    bind(kConfigCtx, tap(button_ref(Button::F7), kShift), kConfigPageDefault),  // OS-SHIFTROW: DEFAULTS, twice
    bind(kConfigCtx, tap(button_ref(Button::F7)), kParamApply),                      // OS-USBAUDIO: APPLY, twice (USB AUDIO and USB RETURN re-enumerate the USB port)
    bind(kConfigCtx, tap(button_ref(Button::F8)), kFirmwareOpen),                    // OS-FWCONFIG: UPDATE on the INFO page only (the action does nothing elsewhere); the dialog is below

    // ---- Mixer (OS-011b): the Master button's four pages. The encoder edits what the cursor is on (Shift: the coarse step; on ROUTING a turn up sets the route bit and a turn down
    // clears it; on STEMS the same for a source of the slot), Delete is the default, F1..F4 are the pages (MIXER, ROUTING, MASTER FX, STEMS), F5 the CUE level, F7 MUTE and F8 SOLO.
    // The arrows are Global's (the Cursor) -------------------------------------------------------------------------------------------------------------------------------
    bind(kMixerCtx, tap(encoder_ref(0)), kMixerEdit, ArgFrom::Value),
    bind(kMixerCtx, tap(encoder_ref(0), kShift), kMixerCoarse, ArgFrom::Value),
    bind(kMixerCtx, tap(button_ref(Button::Delete)), kMixerReset),
    bind(kMixerCtx, tap(button_ref(Button::Delete), kShift), kMixerReset, ArgFrom::Fixed, 1),
    bind(kMixerCtx, tap(button_ref(Button::Insert)), kMixerStemSet),
    bind(kMixerCtx, tap(button_ref(Button::F1)), kMixerPage, ArgFrom::Fixed, 0),
    bind(kMixerCtx, tap(button_ref(Button::F2)), kMixerPage, ArgFrom::Fixed, 1),
    bind(kMixerCtx, tap(button_ref(Button::F3)), kMixerPage, ArgFrom::Fixed, 2),
    bind(kMixerCtx, tap(button_ref(Button::F4)), kMixerPage, ArgFrom::Fixed, 3),
    bind(kMixerCtx, tap(button_ref(Button::F5)), kMixerCue),
    bind(kMixerStemsCtx, tap(button_ref(Button::F6)), kMixerApply),  // OS-USBOUTS: STEMS page only (its field level): APPLY the pending OUTPUTS value (twice)
    bind(kMixerCtx, tap(button_ref(Button::F7)), kMixerMute),
    bind(kMixerCtx, tap(button_ref(Button::F8)), kMixerSolo),
    bind(kMixerCtx, tap(button_ref(Button::F1), kShift), kMixerName),  // OS-NAMES: NAME, the track under the cursor
    bind(kMixerCtx, tap(button_ref(Button::F7), kShift), kMixerMuteNone),  // OS-SHIFTROW: UNMUTE ALL / UNSOLO ALL, one undo unit each
    bind(kMixerCtx, tap(button_ref(Button::F8), kShift), kMixerSoloNone),

    // ---- Perform (OS-011b): the performance mode (seq/perform.h has its rules). The 48 pads are the twelve slots' four rows (pad n: slot n % 12, row n / 12; the release is the
    // App's, as a raw event), the encoder changes the live value of the held slots, Rec is the EDIT state, in which Left / Right move the edit place (Shift: swap with the neighbour), Up / Down
    // change the slot's FX, Copy copies the slot and Shift + Copy pastes it. F1..F8 are OS-PERF2's (scenes, half / double time, latch, next pattern, reset). The tracks the layer affects are the TRACKS layer's row 1: a lone Shift hold turns the pads
    // into AFFECTED / MUTE / MUTE MODE / SOLO (OS-PADS-A, the PadsTracks rows at the end of the table). Play and the screen buttons are Global's ---------------------------------------
    bind(kPerformCtx, tap(pad_ref(core::kAnyId)), kPerformPad, ArgFrom::InputId, 0),
    bind(kPerformCtx, tap(encoder_ref(0)), kPerformTurn, ArgFrom::Value),
    bind(kPerformCtx, tap(encoder_ref(0), kShift), kPerformTurn, ArgFrom::Value),
    bind(kPerformCtx, tap(button_ref(Button::F1)), kPerformKey, ArgFrom::Fixed, 0),  // OS-PERF2: F1..F3 scenes, F4 half, F5 double (the release is the App's), F6 latch, F7 next, F8 reset
    bind(kPerformCtx, tap(button_ref(Button::F2)), kPerformKey, ArgFrom::Fixed, 1),
    bind(kPerformCtx, tap(button_ref(Button::F3)), kPerformKey, ArgFrom::Fixed, 2),
    bind(kPerformCtx, tap(button_ref(Button::F4)), kPerformKey, ArgFrom::Fixed, 3),
    bind(kPerformCtx, tap(button_ref(Button::F5)), kPerformKey, ArgFrom::Fixed, 4),
    bind(kPerformCtx, tap(button_ref(Button::F6)), kPerformKey, ArgFrom::Fixed, 5),
    bind(kPerformCtx, tap(button_ref(Button::F7)), kPerformKey, ArgFrom::Fixed, 6),
    bind(kPerformCtx, tap(button_ref(Button::F8)), kPerformKey, ArgFrom::Fixed, 7),
    bind(kPerformCtx, tap(button_ref(Button::F1), kShift), kPerformKey, ArgFrom::Fixed, 0x100),  // OS-SHIFTROW: STORE 1..3, the scene stored at once (the ~1 s hold of F1..F3 stays)
    bind(kPerformCtx, tap(button_ref(Button::F2), kShift), kPerformKey, ArgFrom::Fixed, 0x101),
    bind(kPerformCtx, tap(button_ref(Button::F3), kShift), kPerformKey, ArgFrom::Fixed, 0x102),
    bind(kPerformCtx, tap(button_ref(Button::F4), kShift), kPerformKey, ArgFrom::Fixed, 0x103),  // E-PERF-07: PICK -, PICK +, TRK PAT, TRK NEXT, TRK SONG (a per-track queued pattern on the tracks that are on)
    bind(kPerformCtx, tap(button_ref(Button::F5), kShift), kPerformKey, ArgFrom::Fixed, 0x104),
    bind(kPerformCtx, tap(button_ref(Button::F6), kShift), kPerformKey, ArgFrom::Fixed, 0x105),
    bind(kPerformCtx, tap(button_ref(Button::F7), kShift), kPerformKey, ArgFrom::Fixed, 0x106),
    bind(kPerformCtx, tap(button_ref(Button::F8), kShift), kPerformKey, ArgFrom::Fixed, 0x107),
    bind(kPerformCtx, tap(button_ref(Button::Rec)), kPerformEdit),
    bind(kPerformCtx, tap(button_ref(Button::Left)), kPerformMove, ArgFrom::Fixed, -1),
    bind(kPerformCtx, tap(button_ref(Button::Right)), kPerformMove, ArgFrom::Fixed, 1),
    bind(kPerformCtx, tap(button_ref(Button::Left), kShift), kPerformSwap, ArgFrom::Fixed, -1),
    bind(kPerformCtx, tap(button_ref(Button::Right), kShift), kPerformSwap, ArgFrom::Fixed, 1),
    bind(kPerformCtx, tap(button_ref(Button::Up)), kPerformFx, ArgFrom::Fixed, 1),
    bind(kPerformCtx, tap(button_ref(Button::Down)), kPerformFx, ArgFrom::Fixed, -1),
    bind(kPerformCtx, tap(button_ref(Button::Copy)), kPerformCopy),
    bind(kPerformCtx, tap(button_ref(Button::Copy), kShift), kPerformPaste),
    // ---- OS-DJ A1: the DJ page of Perform, the screen's field level (the slots' handlers are inert there). The encoder is the crossfader (Shift: one position a detent), the pads' row 4 jump it, F3 CURVE, F6 LATCH and F8 RESET, F4 held + the encoder the filter;
    // the other keys are the later slices' and fall through to the slots' keys, which do nothing on this page. Master + the encoder stays the master volume (a chord of Global). ----
    bind(kPerformDjCtx, tap(pad_ref(core::kAnyId)), kDjPad, ArgFrom::InputId, 0),
    bind(kPerformDjCtx, tap(encoder_ref(0)), kDjTurn, ArgFrom::Value),
    bind(kPerformDjCtx, tap(encoder_ref(0), kShift), kDjFine, ArgFrom::Value),
    bind(kPerformDjCtx, tap(button_ref(Button::F1)), kDjKey, ArgFrom::Fixed, 0),  // OS-DJ A4: BANK - / BANK +; the arrows nudge the tempo (held; the release is the App's), Shift + the arrow is 0.1 BPM
    bind(kPerformDjCtx, tap(button_ref(Button::F2)), kDjKey, ArgFrom::Fixed, 1),
    bind(kPerformDjCtx, tap(button_ref(Button::Left)), kDjNudge, ArgFrom::Fixed, -1),
    bind(kPerformDjCtx, tap(button_ref(Button::Right)), kDjNudge, ArgFrom::Fixed, 1),
    bind(kPerformDjCtx, tap(button_ref(Button::Left), kShift), kDjTempo, ArgFrom::Fixed, -1),
    bind(kPerformDjCtx, tap(button_ref(Button::Right), kShift), kDjTempo, ArgFrom::Fixed, 1),
    bind(kPerformDjCtx, tap(button_ref(Button::F3)), kDjKey, ArgFrom::Fixed, 2),
    bind(kPerformDjCtx, tap(button_ref(Button::F4)), kDjKey, ArgFrom::Fixed, 3),  // OS-QAFIX-14: a tap says how the filter is played
    bind(kPerformDjCtx, hold_encoder(encoder_ref(0), button_ref(Button::F4)), kDjFilter, ArgFrom::Value),  // OS-DJ A2: F4 held + the encoder is the filter; the release is the App's (a raw event: springs back)
    bind(kPerformDjCtx, tap(button_ref(Button::F5)), kDjKey, ArgFrom::Fixed, 4),  // OS-DJ C1: SCR
    bind(kPerformDjCtx, tap(button_ref(Button::F6)), kDjKey, ArgFrom::Fixed, 5),
    bind(kPerformDjCtx, tap(button_ref(Button::F8)), kDjKey, ArgFrom::Fixed, 7),

    // ---- Record (OS-011c): the sample recorder (recorder.h has the rules). The encoder changes the row under the cursor (Shift: coarse), Delete puts the row's default
    // back; F1 and Rec arm or stop the take, F2 is the monitor, F3 saves the take to the first free instrument, F4 throws it away, F5 / F6 seek on the RADIO page, F7 is the WAV file
    // that needs the card's write layer. Up / Down are Global's (the Cursor: the row), the screen button again is the next page --------------------------------------------
    bind(kRecordCtx, tap(encoder_ref(0)), kRecordEdit, ArgFrom::Value),
    bind(kRecordCtx, tap(encoder_ref(0), kShift), kRecordCoarse, ArgFrom::Value),
    // OS-CFG-ARROWS (new Community OS behaviour, replaces OS-SHIFTROW rule 5 here): Left / Right are the encoder's detent on the row, Shift + Left / Right its coarse step; the pages are the screen button.
    bind(kRecordCtx, tap(button_ref(Button::Left)), kRecordEdit, ArgFrom::Fixed, -1),
    bind(kRecordCtx, tap(button_ref(Button::Right)), kRecordEdit, ArgFrom::Fixed, 1),
    bind(kRecordCtx, tap(button_ref(Button::Left), kShift), kRecordCoarse, ArgFrom::Fixed, -1),
    bind(kRecordCtx, tap(button_ref(Button::Right), kShift), kRecordCoarse, ArgFrom::Fixed, 1),
    bind(kRecordCtx, tap(button_ref(Button::Delete)), kRecordReset),
    bind(kRecordCtx, tap(pad_ref(core::kAnyId)), kRecordPad, ArgFrom::InputId, 0),  // OS-REC2: the pads play the take under review (record_view.h)
    bind(kRecordCtx, tap(button_ref(Button::F1)), kRecordToggle),
    bind(kRecordCtx, tap(button_ref(Button::Rec)), kRecordToggle),
    bind(kRecordCtx, tap(button_ref(Button::F2)), kRecordMonitor),
    bind(kRecordCtx, tap(button_ref(Button::F3)), kRecordSave),
    bind(kRecordCtx, tap(button_ref(Button::F3), kShift), kRecordCrop),  // OS-RECCROP: Shift + F3 CROP, next to SAVE: both act on the take's window
    bind(kRecordCtx, tap(button_ref(Button::F4)), kRecordDiscard),
    bind(kRecordCtx, tap(button_ref(Button::F5)), kRecordSeek, ArgFrom::Fixed, -1),
    bind(kRecordCtx, tap(button_ref(Button::F6)), kRecordSeek, ArgFrom::Fixed, 1),
    bind(kRecordRadioCtx, tap(button_ref(Button::F5), kShift), kRecordSeek, ArgFrom::Fixed, -10),  // OS-SHIFTROW: CH-10 / CH+10, ten channels; the RADIO page's own level, blank on the others
    bind(kRecordRadioCtx, tap(button_ref(Button::F6), kShift), kRecordSeek, ArgFrom::Fixed, 10),
    bind(kRecordCtx, tap(button_ref(Button::F7)), kRecordWav),
    bind(kRecordCtx, tap(button_ref(Button::F8)), kRecordPreview, ArgFrom::Fixed, static_cast<int>(Button::F8)),  // OS-FU3: the take's held PREVIEW

    // ---- Instrument PARAMS (OS-002d, OS-INSTUI S1/S2): the encoder edits the row under the cursor (Shift: the coarse step), Delete puts it back to its default, F1..F5 are the pages of the instrument
    // in view (F1 the source page, F2..F5 VOICE / FILTER / MOD / SENDS; a module's F1..F4 are its own pages; instrument.page decides), F6 / F7 step the instrument (Shift: the nearest in use), F8 is UNDO and
    // Shift + F8 REDO as on Pattern and Song, a pad plays the instrument and Shift + pad selects it without a sound. Shift + F1..F5 are NAME, COPY, PASTE, DELETE and SAVE (S5, S7). Left / Right
    // edit the row (OS-INSTUI-ARROWS) and Up / Down move it: Up / Down are Global's (the Cursor) ---------------------------------------------------------------------------------------------------------
    bind(kInstrumentCtx, tap(encoder_ref(0)), kParamEdit, ArgFrom::Value),
    bind(kInstrumentCtx, tap(encoder_ref(0), kShift), kParamCoarse, ArgFrom::Value),
    bind(kInstrumentCtx, tap(button_ref(Button::Delete)), kParamReset),
    // OS-INSTUI-ARROWS: Left / Right are the encoder's detent on the row (-1 / +1; an auto-repeat is one tap per detent), Shift + Left / Right its coarse step. The pages are the F-keys (instrument.page:
    // the same key again steps the windows of its group). The empty slot's chooser keeps instrument.turn.
    bind(kInstrumentCtx, tap(button_ref(Button::Left)), kParamEdit, ArgFrom::Fixed, -1),
    bind(kInstrumentCtx, tap(button_ref(Button::Right)), kParamEdit, ArgFrom::Fixed, 1),
    bind(kInstrumentCtx, tap(button_ref(Button::Left), kShift), kParamCoarse, ArgFrom::Fixed, -1),
    bind(kInstrumentCtx, tap(button_ref(Button::Right), kShift), kParamCoarse, ArgFrom::Fixed, 1),
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::Left)), kInstrumentTurn, ArgFrom::Fixed, -1),  // the pages of the chooser's slot, wrapping on a fresh press (an auto-repeat stops at the end)
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::Right)), kInstrumentTurn, ArgFrom::Fixed, 1),
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::Left), kShift), kInstrumentTurn, ArgFrom::Fixed, -1),  // (Shift + arrow is the coarse step of a used slot; here it is the plain arrow, there is no value to step)
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::Right), kShift), kInstrumentTurn, ArgFrom::Fixed, 1),
    bind(kInstrumentCtx, tap(pad_ref(core::kAnyId)), kInstrumentPad, ArgFrom::InputId, 0),
    bind(kInstrumentCtx, tap(pad_ref(core::kAnyId), kShift), kInstrument, ArgFrom::InputId, 1),  // Shift + pad selects the instrument without a sound, as Wave's (O3)
    bind(kInstrumentCtx, hold(button_ref(Button::Instr)), kPopupOpen, ArgFrom::Fixed, 0),  // OS-FU3: the instrument list
    bind(kInstrumentCtx, tap(button_ref(Button::F1)), kInstrumentPage, ArgFrom::Fixed, 0),
    bind(kInstrumentCtx, tap(button_ref(Button::F2)), kInstrumentPage, ArgFrom::Fixed, 1),
    bind(kInstrumentCtx, tap(button_ref(Button::F3)), kInstrumentPage, ArgFrom::Fixed, 2),
    bind(kInstrumentCtx, tap(button_ref(Button::F4)), kInstrumentPage, ArgFrom::Fixed, 3),
    bind(kInstrumentCtx, tap(button_ref(Button::F5)), kInstrumentPage, ArgFrom::Fixed, 4),
    // The type chooser of an empty slot (OS-INSTUI S5; the screen's field level, so F1..F5 mean these there and the pages elsewhere): SAMPLE, MIDI, PASTE (S7), MODULE (the picker) and LOAD (S7).
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::F1)), kInstrumentSample),
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::F2)), kInstrumentNewMidi),
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::F3)), kInstrumentPaste),
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::F4)), kModuleEngine),
    bind(kInstrumentEmptyCtx, tap(button_ref(Button::F5)), kInstrumentLoad),
    bind(kInstrumentCtx, tap(button_ref(Button::F6)), kInstrumentStep, ArgFrom::Fixed, -1),
    bind(kInstrumentCtx, tap(button_ref(Button::F7)), kInstrumentStep, ArgFrom::Fixed, 1),
    bind(kInstrumentCtx, tap(button_ref(Button::F8)), kUndo),
    bind(kInstrumentCtx, tap(button_ref(Button::F1), kShift), kInstrumentName),
    bind(kInstrumentCtx, tap(button_ref(Button::F2), kShift), kInstrumentCopy),
    bind(kInstrumentCtx, tap(button_ref(Button::F3), kShift), kInstrumentPaste),
    bind(kInstrumentCtx, tap(button_ref(Button::F4), kShift), kInstrumentDelete),  // OS-INSTUI S5
    bind(kInstrumentCtx, tap(button_ref(Button::Delete), kShift), kInstrumentInit),
    bind(kInstrumentCtx, tap(button_ref(Button::F5), kShift), kInstrumentSave),
    bind(kInstrumentCtx, tap(button_ref(Button::F6), kShift), kInstrumentUsed, ArgFrom::Fixed, -1),
    bind(kInstrumentCtx, tap(button_ref(Button::F7), kShift), kInstrumentUsed, ArgFrom::Fixed, 1),
    bind(kInstrumentCtx, tap(button_ref(Button::F8), kShift), kRedo),

    // ---- The choosers (captures): a strip over the info line. Up / Down and the encoder step by one, Left / Right by the defined values,
    // a pad picks (held while playing, the PATTERN chooser's pick switches the playing pattern: OS-PATSWITCH); the F-key that opened LENGTH or JUMP by a tap again closes it (or another F-key switches), F8 closes, the release of a held F-key closes
    // what that hold opened. As F1..F3 held -----------------------------------------------
    bind(kChooserCtx, tap(button_ref(Button::Up)), kChooserDelta, ArgFrom::Fixed, 1),
    bind(kChooserCtx, tap(button_ref(Button::Down)), kChooserDelta, ArgFrom::Fixed, -1),
    bind(kChooserCtx, tap(button_ref(Button::Left)), kChooserDefined, ArgFrom::Fixed, -1),
    bind(kChooserCtx, tap(button_ref(Button::Right)), kChooserDefined, ArgFrom::Fixed, 1),
    bind(kChooserCtx, tap(encoder_ref(0)), kChooserDelta, ArgFrom::Value),
    bind(kChooserCtx, tap(encoder_ref(0), kShift), kChooserCoarse, ArgFrom::Value),  // OS-SHIFTENC: 8 places
    bind(kChooserCtx, tap(pad_ref(core::kAnyId)), kChooserPad, ArgFrom::InputId, 0),
    bind(kChooserCtx, tap(button_ref(Button::F1)), kChooserToggle, ArgFrom::Fixed, 0),
    bind(kChooserCtx, tap(button_ref(Button::F2)), kChooserToggle, ArgFrom::Fixed, 1),
    bind(kChooserCtx, tap(button_ref(Button::F3)), kChooserToggle, ArgFrom::Fixed, 2),
    bind(kChooserCtx, hold_end(button_ref(Button::F1)), kChooserRelease, ArgFrom::Fixed, 0),
    bind(kChooserCtx, hold_end(button_ref(Button::F2)), kChooserRelease, ArgFrom::Fixed, 1),
    bind(kChooserCtx, hold_end(button_ref(Button::F3)), kChooserRelease, ArgFrom::Fixed, 2),
    bind(kChooserCtx, tap(button_ref(Button::F8)), kBack),
    // PAT-OPS-B: with F1 held (the PATTERN chooser; the actions do nothing in the LENGTH and JUMP choosers, which share this context): Copy takes the chooser's pattern whole, Shift + Copy writes it over the chooser's pattern.
    bind(kChooserCtx, tap(button_ref(Button::Copy)), kPatternClipCopy),
    bind(kChooserCtx, tap(button_ref(Button::Copy), kShift), kPatternClipPaste),

    // ---- Learn overlay (captures): the chord again, or F8; F1 unbinds the target, F2 changes how its controller moves it; the list of bindings: arrows or the encoder
    // choose, F3 deletes the chosen one, F4 clears them all (pressed twice) -----------------------------------------------------
    bind(kLearnCtx, chord(button_ref(Button::Vol), button_ref(Button::Fx)), kLearn),
    bind(kLearnCtx, tap(button_ref(Button::F1)), kLearnClear),
    bind(kLearnCtx, tap(button_ref(Button::F2)), kLearnMode),
    bind(kLearnCtx, tap(button_ref(Button::F3)), kLearnDelete),
    bind(kLearnCtx, tap(button_ref(Button::F4)), kLearnClearAll),
    bind(kLearnCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kLearnCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kLearnCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kLearnCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC
    bind(kLearnCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),  // OS-NAV3: a page of the list
    bind(kLearnCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kLearnCtx, tap(button_ref(Button::F8)), kBack),

    // ---- Navigator overlay (captures; OS-NAV2: it stays open after the keys that opened it are let go): choose with the arrows or the encoder; F8 GO opens the chosen screen
    // (F1 too); F7 or Pattern again CLOSE it without changing the view. F8 is Back in the other overlays, so its footer says F7 CLOSE / F8 GO. Shift + F1..F5 jump around the grid
    // like everywhere else. Nothing happens on a release: no HoldEnd, no Shift release ---------------------------------------
    bind(kNavigatorCtx, tap(button_ref(Button::Left)), kNavLeft),
    bind(kNavigatorCtx, tap(button_ref(Button::Right)), kNavRight),
    bind(kNavigatorCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kNavigatorCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kNavigatorCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kNavigatorCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC
    bind(kNavigatorCtx, tap(button_ref(Button::F8)), kNavigatorAccept),
    bind(kNavigatorCtx, tap(button_ref(Button::F1)), kNavigatorAccept),
    bind(kNavigatorCtx, tap(button_ref(Button::F7)), kBack),
    bind(kNavigatorCtx, tap(button_ref(Button::Pattern)), kBack),  // (with Shift still down it falls back to this one; OS-ZOOM's tap of Pattern on Pattern will be Pattern's own binding, behind this context, which captures)
    // OS-SHIFTROW: no quarter jumps here (the Navigator's grid is moved by the arrows). Shift is usually still down from the Shift + Pattern that opened it, so Shift + F7 / F8 stay CLOSE / GO, bound.
    bind(kNavigatorCtx, tap(button_ref(Button::F7), kShift), kBack),
    bind(kNavigatorCtx, tap(button_ref(Button::F8), kShift), kNavigatorAccept),
    bind(kNavigatorCtx, tap(button_ref(Button::F1), kShift), kUiNone),
    bind(kNavigatorCtx, tap(button_ref(Button::F2), kShift), kUiNone),
    bind(kNavigatorCtx, tap(button_ref(Button::F3), kShift), kUiNone),
    bind(kNavigatorCtx, tap(button_ref(Button::F4), kShift), kUiNone),
    bind(kNavigatorCtx, tap(button_ref(Button::F5), kShift), kUiNone),
    bind(kNavigatorCtx, tap(button_ref(Button::F6), kShift), kUiNone),

    // ---- MODULES (OS-012b): F1 launches the app under the cursor, F2 reads the card again; the arrows and the encoder are Global's (the Cursor) ----------------------
    bind(kModulesCtx, tap(button_ref(Button::F1)), kModuleLaunch),
    bind(kModulesCtx, tap(button_ref(Button::F2)), kModuleRescan),
    bind(kModulesCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),  // OS-NAV3: a page of the list
    bind(kModulesCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kModulesCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC

    // ---- An app's page (OS-012b; the context captures: the app gets the input raw, before the bindings, and nothing else acts). The OS keeps the exit chord F7 + F8, which works
    // whatever the module does, and the Shift layer of the app's parameters: Shift + Up / Down the row, Shift + the encoder edits it, Shift + Delete its default ------------
    bind(kModuleAppCtx, chord(button_ref(Button::F7), button_ref(Button::F8)), kModuleExit),
    bind(kModuleAppCtx, tap(button_ref(Button::F8)), kModuleExit),                         // OS-KEYROWDLG S5: F8 is BACK on every app page; the app never sees it
    bind(kModuleAppCtx, tap(button_ref(Button::F8), kShift), kUiNone),                     // Shift + F8 reads nothing and does nothing (it must not fall through to BACK)
    // OS-APPUX S3 (ABI 1.10 slice): a screen button leaves the app and opens its screen (the page used to swallow them): the app is ended as F8 ends it (Screen::leave: exit, shutdown), what it keeps is what
    // set_own_param stored. The app never sees these buttons.
    bind(kModuleAppCtx, tap(button_ref(Button::Pattern)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Pattern)),
    bind(kModuleAppCtx, tap(button_ref(Button::Song)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Song)),
    bind(kModuleAppCtx, tap(button_ref(Button::Params)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Instrument)),
    bind(kModuleAppCtx, tap(button_ref(Button::SamplePlay)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Wave)),
    bind(kModuleAppCtx, tap(button_ref(Button::SampleEdit)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::SampleEdit)),
    bind(kModuleAppCtx, tap(button_ref(Button::SampleLoad)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::SampleLoad)),
    bind(kModuleAppCtx, tap(button_ref(Button::SampleRec)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Record)),
    bind(kModuleAppCtx, tap(button_ref(Button::Master)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Mixer)),
    bind(kModuleAppCtx, tap(button_ref(Button::Performance)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Perform)),
    bind(kModuleAppCtx, tap(button_ref(Button::File)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::File)),
    bind(kModuleAppCtx, tap(button_ref(Button::Config)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Config)),
    bind(kModuleAppCtx, tap(button_ref(Button::Config), kShift), kHelpOpen),                // OS-HELP: the page captures, so Global's chord is bound here too (the app is not given the press)
    bind(kModuleAppCtx, tap(button_ref(Button::Play)), kTransportPlay),                    // the page captures, so the global Play is bound here: the transport runs, the app never sees Play
    bind(kModuleAppCtx, tap(button_ref(Button::Up), kShift), kNavUp),
    bind(kModuleAppCtx, tap(button_ref(Button::Down), kShift), kNavDown),
    bind(kModuleAppCtx, tap(encoder_ref(0), kShift), kModuleAppEdit, ArgFrom::Value),
    bind(kModuleAppCtx, tap(button_ref(Button::Delete), kShift), kModuleAppReset),

    // ---- The engine picker (captures): the arrows and the encoder choose, F1 binds the module to the instrument, F8 closes ----------------------------------------------
    bind(kModulePickerCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kModulePickerCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kModulePickerCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kModulePickerCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC
    bind(kModulePickerCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),  // OS-NAV3: a page of the list
    bind(kModulePickerCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kModulePickerCtx, tap(button_ref(Button::F1)), kModulePick),
    bind(kModulePickerCtx, tap(button_ref(Button::F8)), kBack),

    // ---- The firmware update dialog (OS-FWCONFIG, captures): Up / Down and the encoder choose a file, F1 answers (UPDATE, YES, INSTALL), F8 goes back a step (BACK on the list). Any other control takes an open question back (App::run_once).
    bind(kFirmwareCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kFirmwareCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kFirmwareCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kFirmwareCtx, tap(button_ref(Button::F1)), kFirmwareGo),
    bind(kFirmwareCtx, tap(button_ref(Button::F1), kShift), kUiNone),  // only F1 alone answers (spec risk 6): Shift + F1 does nothing, and App::run_once takes the open question back
    bind(kFirmwareCtx, tap(button_ref(Button::F8)), kFirmwareBack),

    // ---- The TOOLS list (OS-H3A, captures): the arrows and the encoder choose, F1 runs the item, F8 closes ------------------------------------------------------------------------------
    bind(kToolsCtx, tap(button_ref(Button::Up)), kNavUp),
    bind(kToolsCtx, tap(button_ref(Button::Down)), kNavDown),
    bind(kToolsCtx, tap(encoder_ref(0)), kNavScroll, ArgFrom::Value),
    bind(kToolsCtx, tap(encoder_ref(0), kShift), kPage, ArgFrom::Value),  // OS-SHIFTENC
    bind(kToolsCtx, tap(button_ref(Button::Up), kShift), kPage, ArgFrom::Fixed, -1),
    bind(kToolsCtx, tap(button_ref(Button::Down), kShift), kPage, ArgFrom::Fixed, 1),
    bind(kToolsCtx, tap(button_ref(Button::F1)), kToolsRun),
    bind(kToolsCtx, tap(button_ref(Button::F8)), kBack),

    // ---- The on-screen keyboard (OS-FU2; captures: the pads type their key, the arrows move the focus, Insert and F1 type the key in
    // focus, Delete is the backspace, F6 the automatic name, F7 cancels, F8 saves; F2 is the caps key, the encoder moves the focus along the row) -------------------------------
    bind(kTextEntryCtx, tap(pad_ref(core::kAnyId)), kTextPad, ArgFrom::InputId, 0),
    bind(kTextEntryCtx, tap(button_ref(Button::Left)), kTextMove, ArgFrom::Fixed, 0),
    bind(kTextEntryCtx, tap(button_ref(Button::Right)), kTextMove, ArgFrom::Fixed, 1),
    bind(kTextEntryCtx, tap(button_ref(Button::Up)), kTextMove, ArgFrom::Fixed, 2),
    bind(kTextEntryCtx, tap(button_ref(Button::Down)), kTextMove, ArgFrom::Fixed, 3),
    bind(kTextEntryCtx, tap(encoder_ref(0)), kTextScroll, ArgFrom::Value),
    bind(kTextEntryCtx, tap(button_ref(Button::Insert)), kTextKey),
    bind(kTextEntryCtx, tap(button_ref(Button::F1)), kTextKey),
    bind(kTextEntryCtx, tap(button_ref(Button::F2)), kTextCaps),
    bind(kTextEntryCtx, tap(button_ref(Button::Delete)), kTextBackspace),
    bind(kTextEntryCtx, tap(button_ref(Button::F6)), kTextAuto),
    bind(kTextEntryCtx, tap(button_ref(Button::F7)), kBack),
    bind(kTextEntryCtx, tap(button_ref(Button::F8)), kTextAccept),

    // ---- The step popup (OS-FU3): the arrows (dir codes 1 Up, 2 Right, 3 Down, 4 Left), the encoder and the pads choose, the release of the button that opened it closes ----
    bind(kStepPopupCtx, tap(button_ref(Button::Up)), kPopupMove, ArgFrom::Fixed, 1),
    bind(kStepPopupCtx, tap(button_ref(Button::Right)), kPopupMove, ArgFrom::Fixed, 2),
    bind(kStepPopupCtx, tap(button_ref(Button::Down)), kPopupMove, ArgFrom::Fixed, 3),
    bind(kStepPopupCtx, tap(button_ref(Button::Left)), kPopupMove, ArgFrom::Fixed, 4),
    bind(kStepPopupCtx, tap(button_ref(Button::Instr), kNoteKey), kPatternZoom),  // OS-ZOOM: the Note popup's held Note keeps the Note + Instr / Left / Right of the grid underneath
    bind(kStepPopupCtx, tap(button_ref(Button::Left), kNoteKey), kTrack, ArgFrom::Fixed, -1),
    bind(kStepPopupCtx, tap(button_ref(Button::Right), kNoteKey), kTrack, ArgFrom::Fixed, 1),
    bind(kStepPopupCtx, tap(button_ref(Button::Up), kShift), kPopupPage, ArgFrom::Fixed, -1),  // OS-NAV3: a page of the list in view (popup.page)
    bind(kStepPopupCtx, tap(button_ref(Button::Down), kShift), kPopupPage, ArgFrom::Fixed, 1),
    bind(kStepPopupCtx, tap(encoder_ref(0)), kPopupDelta, ArgFrom::Value),
    bind(kStepPopupCtx, tap(encoder_ref(0), kShift), kPopupPage, ArgFrom::Value),  // OS-SHIFTENC: a page of the list in view a detent (the Note panel has none)
    bind(kStepPopupCtx, tap(pad_ref(core::kAnyId)), kPopupPad, ArgFrom::InputId, 0),
    bind(kStepPopupCtx, hold_end(button_ref(Button::Instr)), kPopupRelease, ArgFrom::Fixed, 0),
    bind(kStepPopupCtx, hold_end(button_ref(Button::Fx)), kPopupRelease, ArgFrom::Fixed, 1),
    bind(kStepPopupCtx, hold_end(button_ref(Button::Note)), kPopupRelease, ArgFrom::Fixed, 2),
    bind(kStepPopupCtx, hold_end(button_ref(Button::Vol)), kPopupRelease, ArgFrom::Fixed, 3),
    bind(kStepPopupCtx, tap(button_ref(Button::F8)), kBack),

    // ---- The Fill dialog (OS-PTOOLS; captures): Up / Down choose a row, Left / Right or the encoder change its value, F1 fills (one undo unit), F8 closes -------------------------------------
    bind(kFillCtx, tap(button_ref(Button::Up)), kFillMove, ArgFrom::Fixed, -1),
    bind(kFillCtx, tap(button_ref(Button::Down)), kFillMove, ArgFrom::Fixed, 1),
    bind(kFillCtx, tap(button_ref(Button::Left)), kFillDelta, ArgFrom::Fixed, -1),
    bind(kFillCtx, tap(button_ref(Button::Right)), kFillDelta, ArgFrom::Fixed, 1),
    bind(kFillCtx, tap(encoder_ref(0)), kFillDelta, ArgFrom::Value),
    bind(kFillCtx, tap(encoder_ref(0), kShift), kFillCoarse, ArgFrom::Value),  // OS-SHIFTENC
    bind(kFillCtx, tap(button_ref(Button::F1)), kFillApply),
    bind(kFillCtx, tap(button_ref(Button::F8)), kBack),

    // ---- INPUT TEST (OS-007d; the screen's context captures, so nothing else acts). It sees every control raw; these two chords are
    // the only bindings: F4 + F5 together leave for the Pattern screen, F2 + F3 together clear the counts. Neither is part of the
    // boot chord that opens the screen (Shift + F1 + F8, app_main.cpp), or the held chord would close it at once -----------------
    bind(kInputTestCtx, chord(button_ref(Button::F4), button_ref(Button::F5)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Pattern)),
    bind(kInputTestCtx, chord(button_ref(Button::F2), button_ref(Button::F3)), kInputTestReset),
};

// The TRACKS layer of the pads (OS-PADS-A; pad_layer.h, pad_lights.h): 48 rows in the innermost context PadsTracks, which is in the chain only while the layer is in force (a fixed TRACKS
// mode on Pattern, a lone Shift hold on Pattern and Perform). Pad = 12 * row + track: row 1 the cursor's track (on Perform: AFFECTED), row 2 mute, row 3 the track's mute mode, row 4 solo.
// A binding names one pad or all of them, so they are written one by one.
struct TracksRows {
  core::Binding items[48];
};
constexpr TracksRows make_tracks_rows() noexcept {
  TracksRows rows{};
  for (unsigned pad = 0; pad < 48; ++pad) {
    const core::ActionId action = pad < 12 ? kPadsTrack : (pad < 24 ? kTrackMute : (pad < 36 ? kTrackMuteMode : kTrackSolo));
    rows.items[pad] = bind(kPadsTracksCtx, tap(pad_ref(static_cast<std::uint8_t>(pad))), action, ArgFrom::Fixed, static_cast<ActionArg>(pad % 12));
  }
  return rows;
}
constexpr TracksRows kTracksRows = make_tracks_rows();

template <std::size_t A, std::size_t B>
struct Joined {
  core::Binding items[A + B];
};
template <std::size_t A, std::size_t B>
constexpr Joined<A, B> join(const core::Binding (&a)[A], const core::Binding (&b)[B]) noexcept {
  Joined<A, B> out{};
  for (std::size_t i = 0; i < A; ++i) {
    out.items[i] = a[i];
  }
  for (std::size_t i = 0; i < B; ++i) {
    out.items[A + i] = b[i];
  }
  return out;
}

constexpr auto kBuiltinTable = join(kBase, kTracksRows.items);
constexpr const core::Binding* kBuiltin = kBuiltinTable.items;
constexpr std::size_t kBuiltinCount = sizeof(kBuiltinTable.items) / sizeof(kBuiltinTable.items[0]);
static_assert(!core::find_conflict(kBuiltin, kBuiltinCount).found, "two built-in bindings conflict");

struct ContextName {
  const char* name;
  Context context;
};
// What a keymap line names a context. Modes are "<screen>.<mode>" (the mode's own name, screens/pattern_screen.cpp).
constexpr ContextName kContextNames[] = {
    {"global", Context::Global},
    {"pattern", Context::Pattern},
    {"pattern.edit", Context::PatternEdit},
    {"pattern.follow", Context::PatternFollow},  // retired (OS-004d): still named so that a keymap that uses it loads, but no mode enters it
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
    {"file.export", Context::FileExport},
    {"exportprogress", Context::ExportProgress},
    {"sampleedit", Context::SampleEdit},
    {"sampleedit.fx", Context::SampleEditFx},
    {"config", Context::Config},
    {"mixer", Context::Mixer},
    {"mixer.stems", Context::MixerStems},
    {"perform", Context::Perform},
    {"record", Context::Record},
    {"song", Context::Song},
    {"learn", Context::Learn},
    {"navigator", Context::Navigator},
    {"inputtest", Context::InputTest},
    {"instrument", Context::Instrument},
    {"instrument.empty", Context::InstrumentEmpty},
    {"modules", Context::Modules},
    {"moduleapp", Context::ModuleApp},
    {"modulepicker", Context::ModulePicker},
    {"tools", Context::ToolsList},
    {"textentry", Context::TextEntry},
    {"steppopup", Context::StepPopup},
    {"pads.tracks", Context::PadsTracks},
    {"pads.mode", Context::PadsMode},
    {"fill", Context::FillDialog},
    {"song.ask", Context::SongAsk},
    {"song.tools", Context::SongTools},
    {"firmware", Context::FirmwareUpdate},
    {"help", Context::Help},
    {"record.radio", Context::RecordRadio},
    {"perform.dj", Context::PerformDj},
};

bool equals(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// The three lookups receive a lower-case copy of the word.
bool find_context(const void*, const char* name, core::ContextId& out) noexcept {
  for (const ContextName& c : kContextNames) {
    if (equals(name, c.name)) {
      out = context_id(c.context);
      return true;
    }
  }
  return false;
}

bool find_button(const void*, const char* name, std::uint8_t& out) noexcept {
  for (const ButtonName& b : kButtonNames) {
    if (equals(name, b.name)) {
      out = static_cast<std::uint8_t>(b.button);
      return true;
    }
  }
  return false;
}

bool find_action_name(const void*, const char* name, core::ActionId& out) noexcept {
  std::size_t n = 0;
  while (name[n] != '\0') {
    ++n;
  }
  const ActionDef* a = find_action(action_registry(), name, n);
  if (a == nullptr) {
    return false;
  }
  out = a->id;
  return true;
}

// The buttons a keymap line may name before a `+` (core/keymap.cpp): Shift, the gesture layer's constructor modifier, and Note (and Copy, below), which becomes a held modifier by being named as one (OS-ZOOM, kWantModifier in
// core/gesture.h: the bindings decide, in the contexts that bind it). The keymap file can rebind or switch off the built-in Note + Instr / Left / Right rows or add its own `note+...` rows.
// OS-SONGSHARE S3: and Copy, the same way: Song's built-in Copy + Up / Down rows say `copy+up` / `copy+down`, so the loader must name it (the built-in table is written and read back as keymap text in the tests).
constexpr std::uint64_t kKeymapModifiers = kModifierButtons | (std::uint64_t{1} << static_cast<unsigned>(Button::Note)) | (std::uint64_t{1} << static_cast<unsigned>(Button::Copy));

constexpr core::KeymapNames kNames{nullptr, find_context, find_button, find_action_name, kKeymapModifiers};

}  // namespace

core::BindingView builtin_bindings() noexcept { return core::BindingView{kBuiltin, kBuiltinCount}; }

const core::KeymapNames& keymap_names() noexcept { return kNames; }

}  // namespace o3os::app
