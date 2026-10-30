// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The parameter tables of the model (ADR 0009 rev 1, mechanism 5; OS-002d): which values of a project and of the device are
// parameters, what each one is (core::ParamDesc: range, default, unit, curve, steps, flags, page) and where it is stored.
//
//   kind        index         fields (field ids below)                                    owner struct
//   Instrument  slot 0..63    13 voice, filter, send fields; 6 envelopes x 6 fields        Instrument::sampler
//   Track       slot 0..15    volume, pan, mute, solo, output bus                          TrackSlot
//   Master      0             four levels and four switches                                MasterSettings
//   Project     0             settings that belong to the project                          Settings (model/settings.h)
//   Device      0             settings that belong to the device                           Settings
//
// The tables are the only place a parameter is described. The generated edit page (app/screens/params_screen.h), MIDI Learn,
// the settings files and the undo journal (editor.h) all read them, so a new parameter is one row here: it gets a row on the
// page of its group, can be learned, is stored and journalled, with no code of its own. Every instrument field has the value
// the OS's own fresh instrument has (init_instrument) as its default;
// tests/param_test.cpp holds the equality. Ranges follow the fields of the OG instrument file (docs/formats/og.md); the defaults and the names are
// this OS's choice; each row that is not from the file says so.
//
// Values are floats at this level whatever the storage is (u8, i8, u16 or f32 fields, or one bit of a flag byte): an integer field
// holds the rounded value, a flag is 0 or 1.

#include <o3os/core/learn.h>
#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::model {

// Field ids of kind Instrument (the sampler block). Envelope e (0..5: volume, panning, cutoff, wavetable position, granular
// position, finetune, the instrument file's order) field f is kEnvelopeFirst + e * kEnvelopeStride + f.
namespace instrument_param {
inline constexpr unsigned Volume = 0;
inline constexpr unsigned Panning = 1;
inline constexpr unsigned Tune = 2;
inline constexpr unsigned FineTune = 3;
inline constexpr unsigned Glide = 4;
inline constexpr unsigned FilterEnable = 5;
inline constexpr unsigned FilterType = 6;
inline constexpr unsigned Cutoff = 7;
inline constexpr unsigned Resonance = 8;
inline constexpr unsigned DelaySend = 9;
inline constexpr unsigned ReverbSend = 10;
inline constexpr unsigned Overdrive = 11;
inline constexpr unsigned BitDepth = 12;
// OS-FU5: the instrument's PLAYBACK engine (Instrument::engine, not a sampler field: its row's offset is that of the struct), the INST 1.2 choice Auto / Modern / Amiga / 12-bit /
// 8-bit. Not the module ENGINE picker of Params F1 (an SD module bound to the slot): hence "PLAY ENGINE". Appended, so every other id keeps its number; not learnable. Edits go through
// the engine action (project_state.cpp), which refuses an Amiga the sample's rate cannot play and rebinds the slot.
inline constexpr unsigned PlayEngine = 13;
inline constexpr unsigned kBase = 14;  // the fields of the first table (the sampler's voice, filter and sends, and the playback engine)

// OS-INSTUI S3: how the SOURCE is played (the Params F1 page): the play mode and, by mode, the wavetable's, the grain's and the slices' fields. All are stored already (INST sampler block); these are the rows
// that edit them. Appended past the first table at 0x14.. so that every id above keeps its number (0x10..0x13 are the audio side's live edits, voices.h inst_param: the model's ids must not collide with
// them). Not modulatable: an edit rebinds the slot (the descriptor is rebuilt from the project), and PLAY MODE / WT SIZE / SLICES go through a validating action (project_state.cpp, param_actions.cpp
// source_param) and are not learnable, like PLAY ENGINE; the positions, the grain and the selected slice are (a CC clamps to the sample as the encoder does).
inline constexpr unsigned kSourceFirst = 0x14;
inline constexpr unsigned PlayMode = 0x14;     // SamplerParams::play_mode, the play mode 0..7
inline constexpr unsigned WtPos = 0x15;        // wavetable_window: the window in use (0 .. windows - 1)
inline constexpr unsigned WtSize = 0x16;       // wavetable_window_size: 32 .. 2048, a power of two (0: the engine's 2048)
inline constexpr unsigned GrainPos = 0x17;     // grain_position 0..65535
inline constexpr unsigned GrainLength = 0x18;  // grain_length, 44 .. 44100 samples
inline constexpr unsigned GrainShape = 0x19;   // grain_shape: square, triangle, gauss
inline constexpr unsigned GrainLoop = 0x1A;    // grain_type: forward, backward, ping-pong
inline constexpr unsigned Slices = 0x1B;       // slice_count (a readout: the slices are made on Wave)
inline constexpr unsigned Slice = 0x1C;        // selected_slice
inline constexpr unsigned kSourceRows = 9;
// OS-INSTUI S8a: the MIDI page's CHANNEL (Instrument::midi_channel, 1..16). A VIRTUAL field: it is no row of the tables, so the generation block, INIT (the channel stays), Learn and the
// rest of the table plumbing do not see it and INST is unchanged; only the Params screen's MIDI page and app/param_actions.cpp know it. The descriptor is midi_channel_desc(), the value is
// read from the instrument and an edit writes the whole instrument as one undo record (Editor::set_instrument, as NAME). 0x1E: past the source rows (0x1D stays nothing).
inline constexpr unsigned MidiChannel = 0x1E;
// OS-STEREO step 2 (INST 1.5): the source page's STEREO choice (Instrument::stereo, MONO / STEREO), next to PLAY ENGINE. A VIRTUAL field like MidiChannel (no table row, so the generation block and the arena
// do not grow by a slot per instrument): describe_param answers it (play_stereo_desc), the value is read from the instrument and an edit goes through the stereo action (app/param_actions.cpp), which refuses STEREO
// while the bank holds no side signal for the slot. Not learnable, not modulatable, not in the app SDK. 0x1F: the last id before the envelopes.
inline constexpr unsigned PlayStereo = 0x1F;
constexpr bool is_source_field(unsigned field) noexcept { return field >= kSourceFirst && field < kSourceFirst + kSourceRows; }

inline constexpr unsigned kEnvelopeFirst = 0x20;
inline constexpr unsigned kEnvelopeStride = 8;
inline constexpr unsigned EnvEnable = 0;
inline constexpr unsigned EnvAmount = 1;
inline constexpr unsigned EnvAttack = 2;
inline constexpr unsigned EnvDecay = 3;
inline constexpr unsigned EnvSustain = 4;
inline constexpr unsigned EnvRelease = 5;
inline constexpr unsigned kEnvelopeFields = 6;
inline constexpr unsigned kEnvelopes = 6;
constexpr unsigned envelope(unsigned e, unsigned f) noexcept { return kEnvelopeFirst + e * kEnvelopeStride + f; }

// OS-INSTUI S6, the MOD page (F4): per destination (the six envelope slots) a SOURCE row Off / Envelope / LFO and the slot's LFO. All are stored already (Envelope::enable and ::loop, SamplerParams::lfo[e]);
// these rows edit them. Appended past the envelope table at 0x60.. (the audio side's ids end at 0x4D): slot e field f is kModFirst + e * kModStride + f. SOURCE is not a field of the struct but the
// state of two bytes (enable and loop): Off = not enabled, Envelope = enabled, LFO = enabled with `loop` set; reading a slot with loop set but not
// enabled gives Off, and Off clears both. The engine reads the LFO's shape, speed and amount, and the loop byte, when it builds the slot's descriptor (app/og_samples.cpp make_sample_desc), and
// OS-LFOLIVE: an edit of any of these is ALSO a live ParamChange with the same id (audio::inst_param::lfo), which the render applies to the sounding voices (voices.h LIVE LFO FIELDS); the rebind at the quiet moment
// stays the safety net.
inline constexpr unsigned kModFirst = 0x60;
inline constexpr unsigned kModStride = 4;
inline constexpr unsigned ModSource = 0;
inline constexpr unsigned LfoShape = 1;
inline constexpr unsigned LfoSpeed = 2;
inline constexpr unsigned LfoAmount = 3;
inline constexpr unsigned kModFields = 4;
constexpr unsigned mod(unsigned e, unsigned f) noexcept { return kModFirst + e * kModStride + f; }
constexpr bool is_mod_field(unsigned field) noexcept { return field >= kModFirst && field < kModFirst + kEnvelopes * kModStride; }
inline constexpr unsigned SourceOff = 0;
inline constexpr unsigned SourceEnvelope = 1;
inline constexpr unsigned SourceLfo = 2;
inline constexpr unsigned kAmpLfoSpeeds = 24;    // the amp slot's rate table is the last 24 of the 29
inline constexpr unsigned kOtherLfoSpeeds = 29;  // the tempo-sync rates of the other LFOs

// OS-INSTUI S8b + S9 (INST 1.3, docs/formats/INST-1.3-diff.md): the stored fields of a MIDI instrument and the sample instrument's VEL>, twelve rows appended at 0x80.. (past the MOD rows, which end at 0x77, and the audio side's ids, which end at
// 0x4D). Every one is a byte of the Instrument (instrument.h) and none is modulatable and every one is learnable, as every parameter is (they are read where a note is made, or by the router; PROGRAM is also sent to the render over the ring, see
// app/param_actions.cpp send_param_change). MIDI page (F1): CHANNEL (virtual, above), VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET; CC page (F2): CC A..F; VEL> is on the sample instrument's VOICE page.
inline constexpr unsigned kMidiFirst = 0x80;
inline constexpr unsigned MidiVelocity = 0x80;
inline constexpr unsigned MidiProgram = 0x81;
inline constexpr unsigned MidiOut = 0x82;
inline constexpr unsigned MidiPbRange = 0x83;
inline constexpr unsigned MidiOffset = 0x84;
inline constexpr unsigned kMidiCcFirst = 0x85;   // CC A .. CC F: 0x85 .. 0x8A
inline constexpr unsigned VelSens = 0x8B;
inline constexpr unsigned kMidiRows = 12;
constexpr unsigned midi_cc(unsigned slot) noexcept { return kMidiCcFirst + slot; }
constexpr bool is_midi_row(unsigned field) noexcept { return field >= kMidiFirst && field < kMidiFirst + kMidiRows; }

inline constexpr unsigned kCount = kBase + kSourceRows + kEnvelopes * kEnvelopeFields + kEnvelopes * kModFields + kMidiRows;  // parameters of one instrument: 95 (its generation block: 14 + 9 + 36 + 24 + 12)
// The parameters of a SAMPLE instrument that the app ABI (module/sdk/o3module.h, 1.6) lists: the first 83, as published. The twelve MIDI / VEL> rows are the project's and the screen's, not the SDK's (their group numbers
// would also collide with O3M_PGROUP_MOD), so a published count and listing stay exactly what they were.
inline constexpr unsigned kSdkCount = kCount - kMidiRows;

// OS-012b: the parameters of a MODULE instrument (an SD module bound to the slot, ADR 0002 amendment 1). Parameter i of the module (its descriptor order, at most
// kModuleMax) is field kModuleFirst + i of the instrument's id; its value is the i-th little-endian float32 of Instrument::params (the blob the project already
// stores, so nothing new is saved). They are not rows of the tables above: there is no static descriptor, the module's own descriptors (o3m_param) make the
// standard control (app/module_ui.h) and the app clamps to them before it writes; the Project only holds the number (finite) and counts its generation in slot
// i of the instrument's block (a module instrument has no sampler fields, so the slots are free).
inline constexpr unsigned kModuleFirst = 0x100;
inline constexpr unsigned kModuleMax = 32;
constexpr bool is_module_field(unsigned field) noexcept { return field >= kModuleFirst && field < kModuleFirst + kModuleMax; }

// The pages of the generated edit page (ParamDesc::group).
inline constexpr unsigned GroupVoice = 0;
inline constexpr unsigned GroupFilter = 1;
inline constexpr unsigned GroupEnvelope = 2;
inline constexpr unsigned GroupSends = 3;
// OS-INSTUI S3: the F1 source page. Appended (the groups keep their numbers); the key order is SOURCE, VOICE, FILTER, ENV, SENDS (the screen maps keys and Left / Right to it).
inline constexpr unsigned GroupSource = 4;
inline constexpr unsigned kGroups = 5;
// OS-INSTUI S8b: the two pages of a MIDI instrument. They are no groups of a sample instrument (kGroups stays 5: the tables' pages), only the numbers a MIDI instrument's rows carry in ParamDesc::group and
// the screen's page numbers: F1 MIDI (CHANNEL and the five rows above), F2 CC.
inline constexpr unsigned GroupMidi = 5;
inline constexpr unsigned GroupCc = 6;
}  // namespace instrument_param

namespace track_param {
inline constexpr unsigned Volume = 0;
inline constexpr unsigned Pan = 1;
inline constexpr unsigned Mute = 2;
inline constexpr unsigned Solo = 3;
inline constexpr unsigned Bus = 4;
inline constexpr unsigned kCount = 5;
}  // namespace track_param

namespace master_param {
inline constexpr unsigned Volume = 0;
inline constexpr unsigned Dry = 1;
inline constexpr unsigned Reverb = 2;
inline constexpr unsigned Delay = 3;
inline constexpr unsigned DryMute = 4;
inline constexpr unsigned ReverbMute = 5;
inline constexpr unsigned DelayMute = 6;
inline constexpr unsigned Limiter = 7;
inline constexpr unsigned kCount = 8;
}  // namespace master_param

// Settings of scope project (ADR 0009 rev 1; triage D-1: the settings the 1.9.2 manual puts under Project Settings are stored per
// project). Anti-aliasing and the limiter mode are settings of those names that the official manual puts
// under Project Settings; sends mode and snap to zero are 1.2.0 and 1.7.0 items (PAR-024, PAR-111).
namespace project_setting {
inline constexpr unsigned AntiAliasing = 0;
inline constexpr unsigned LimiterMode = 1;
inline constexpr unsigned SendsMode = 2;
inline constexpr unsigned kCount = 3;
}  // namespace project_setting

// Settings of scope device: the config items with a decided range and three the catalog names
// (PAR-120 numbering, PAR-084 middle C, PAR-119 clock sync correction).
namespace device_setting {
inline constexpr unsigned DisplayBrightness = 0;
inline constexpr unsigned PadsBrightness = 1;
inline constexpr unsigned MetronomeVolume = 2;
inline constexpr unsigned ClockIn = 3;
inline constexpr unsigned Numbering = 4;
inline constexpr unsigned MiddleC = 5;
inline constexpr unsigned ClockSync = 6;
// OS-011a (the Config screen): the MIDI routing and the theme. Appended: ids 0..6 keep their meaning, an older settings file has none of these and reads as the defaults.
inline constexpr unsigned ClockMode = 7;
inline constexpr unsigned MidiInPort = 8;
inline constexpr unsigned MidiOutPort = 9;
inline constexpr unsigned MidiInChannel = 10;
inline constexpr unsigned MidiOutChannel = 11;
inline constexpr unsigned Theme = 12;
// OS-USBRET: whether the AUDIO descriptors carry the stereo return stream (USB RETURN: 0 Off, 1 On). Appended: a file written before it has no such entry and reads On.
inline constexpr unsigned UsbReturn = 13;
// OS-LIVEREC: the live recording of the Pattern screen. REC POLY: 0 Off (a note goes to the cursor's track only), 1 Spill (notes held together go to that track, then the next ones, one a voice).
// REC QUANTIZE: 0 Nearest step (the default: a note lands on the step nearest to when it was played), 1 Playing step (the step the playhead is on). Appended, the defaults are 0: a file written before them
// has no such entries and reads Off / Nearest step.
inline constexpr unsigned RecPoly = 14;
inline constexpr unsigned RecQuantize = 15;
// OS-006e: the level of the USB return to the outputs (RETURN LEVEL, 0..100 on the mixer level law, 100 = unity = the default; the USB page of Config). Appended: a file written before it has no such
// entry and reads 100, which is what a host that plays into the Tracker should hear.
inline constexpr unsigned UsbReturnLevel = 16;
// OS-USBAUDIO: whether the unit comes up as the USB audio device (USB AUDIO: 0 Off = STANDARD, MIDI + serial only; 1 On = AUDIO, the stems stream and the return). Appended: a file written before it
// has no such entry and reads On. Applied at boot (the USB port enumerates once, in that mode) and, from Config, on a confirming press (it re-enumerates).
inline constexpr unsigned UsbAudio = 17;
// OS-METRO: the metronome (new Community OS behaviour; docs/specs/OS-METRO.md). METRO: 0 Off, 1 Cue (the default: the click goes to the CUE side of the line out, to MAIN while OUT MODE is Stereo and
// CUE has no output), 2 Main. COUNT-IN: 0 Off (the default), 1 one bar, 2 two bars. METRO VOL (id 2, `device_setting::MetronomeVolume`, this OS's name) moves from the MIDI page to the PATTERN page: a page change only, its id,
// range and default are the same. Appended: a file written before them has no such entries and reads Cue / Off; an older reader skips the two entries it does not know.
inline constexpr unsigned MetroMode = 18;
inline constexpr unsigned CountIn = 19;
// OS-RECMICRO: what a live-recorded note also writes (PAR-037, PAR-064; docs/specs/OS-RECMICRO.md, new Community OS behaviour). REC MICRO: 0 Off (the default), 1 On: the note's offset from its step goes to FX 2 as
// a Micro-move. REC VELOCITY: 0 Off (the default), 1 External: the velocity of a note played on the MIDI keyboard goes to the step's volume column. These two split the official manual's one four-valued recording option (docs/ui/official-manual-map.md).
// Appended, the defaults are 0: a file written before them has no such entries and reads Off / Off; an older reader skips the two entries it does not know.
inline constexpr unsigned RecMicro = 20;
inline constexpr unsigned RecVelocity = 21;
// OS-PADOFFSET: the row offset of the NOTES pad board (PAD OFFSET, 0..12, the Config PATTERN page; new Community OS behaviour: the official manual names a row offset among Config's pad options, its range is not claimed here, official-manual-map). Offset N (0..11) starts a row
// of pads on the Nth note of the row below it; 12, the default and the layout every unit had, continues right after that row's last note. 13, which the board itself still takes, is the layout of 5, so the
// row stops at 12. Appended, the default is 12: a file written before it has no such entry and reads 12; an older reader skips the entry it does not know.
inline constexpr unsigned PadOffset = 22;
// E-CFG-03 (row 13): TRANSPORT IN / TRANSPORT OUT (0 Off, 1 On, the default: today's behaviour for every CLOCK MODE; new Community OS behaviour). IN Off: Start, Continue, Stop and Song Position from the followed port are ignored (the
// clock is still followed). OUT Off: the unit sends no Start, Continue, Stop or Song Position (the clock still goes out). Appended: a file written before them has no such entries and reads On / On; an older reader skips the two
// entries it does not know.
inline constexpr unsigned TransportIn = 23;
inline constexpr unsigned TransportOut = 24;
// E-PAT-35 (row 58; new Community OS behaviour): PAT SWITCH, 0 Step (the default: today's behaviour), 1 Pattern end. While the sequencer plays, a pick in the held PATTERN chooser switches the playing pattern at the next step (Step) or waits for the
// end of the pattern that plays and then starts (Pattern end: the PATS pads' queue). Appended: a file written before it has no such entry and reads Step; an older reader skips the entry it does not know. The device scope is now FULL
// (kCount == Settings::kMaxFields == 26): the next device setting needs another raise in settings.h (kValueBytes, kMaxFields) and in the arena sizes.
inline constexpr unsigned PatSwitch = 25;
inline constexpr unsigned kCount = 26;
}  // namespace device_setting

// EnvState (OS-INSTUI S6): the SOURCE of a modulator slot, 0 Off / 1 Envelope / 2 LFO, kept in two bytes of an Envelope: `loop` at the field's offset and `enable` in the byte after it.
enum class ParamStore : std::uint8_t { U8, I8, U16, F32, EnvState };

// A parameter: its descriptor (static, in the tables) and where it lives, as bytes from the start of its owner struct. `mask` is 0 for a
// whole value and the bit of a flag byte for a switch (the value is then 0 or 1).
struct ParamField {
  const core::ParamDesc* desc;
  std::uint16_t offset;
  ParamStore store;
  std::uint8_t mask;
};

// The parameter named by `id`: false when the kind, the index or the field does not exist (the index of an instrument or a track is
// only range checked here: whether the slot is in use is the project's business). `out.offset` is that of this envelope's field;
// `out.desc` is the table's own descriptor, so it is never null on success and is the same pointer param_desc returns.
bool find_param(core::ParamId id, ParamField& out) noexcept;
// The descriptor alone, static for ever (nullptr when there is no such parameter). The six envelopes share one row for each field,
// except that the volume envelope (0) has its own ENABLE and ATTACK rows: it starts on with a 10 ms attack, the others off at 3 s.
const core::ParamDesc* param_desc(core::ParamId id) noexcept;
// OS-INSTUI S8a: the descriptor of the virtual field instrument_param::MidiChannel (CHANNEL 1..16, default 1, not learnable); param_desc(id) of that field stays nullptr.
const core::ParamDesc& midi_channel_desc() noexcept;
// OS-STEREO step 2: the descriptor of the virtual field instrument_param::PlayStereo (STEREO: MONO / STEREO, default MONO, not learnable); param_desc(id) of that field stays nullptr.
const core::ParamDesc& play_stereo_desc() noexcept;

// Generations are kept in a dense array per kind: how many slots one index of the kind has (instrument 49, track 5, master 8,
// project 3, device 20), and the slot of a parameter in that block (kNoParamSlot if it does not exist).
inline constexpr unsigned kNoParamSlot = 0xFFFFu;
unsigned param_slots_per_index(core::ParamKind kind) noexcept;
unsigned param_slot(core::ParamId id) noexcept;
// Field ids of a slot of the block, the inverse of param_slot (0xFFFF when there is none): what a loop over every parameter of a
// kind calls to get its ids.
unsigned param_field_of_slot(core::ParamKind kind, unsigned slot) noexcept;
unsigned param_index_count(core::ParamKind kind) noexcept;  // 64 instruments, 16 tracks, 1 for the others

// OS-H3B (ABI 1.6): whether an app (a TOOLS app, module/sdk/o3module.h O3M_PD_APP_WRITABLE) may write field `field` of a SAMPLE instrument. Conservative: the sound parameters only. Yes: the voice, filter and
// send rows 0..12 (VOLUME .. BIT DEPTH), and of each of the six envelopes AMOUNT, ATTACK, DECAY, SUSTAIN and RELEASE, and of each modulation slot the LFO's SHAPE, SPEED and AMOUNT. No, because they are
// structural or rebuild the voice: PLAY ENGINE, the whole source page (play mode, wavetable window and size, grain, slices: they go through validating actions), the envelope ENABLE and the SOURCE of a
// modulation slot. A module instrument's parameters are not rows of this table: every one of them is writable (the module declared it, the host clamps to its descriptor).
bool instrument_field_app_writable(unsigned field) noexcept;

// The generated pages. How many groups a kind has, the group's name, and the field ids of the rows of a group in table order:
// for the instrument envelope group `envelope` (0..5) says which of the six. Returns how many were written (at most `cap`).
unsigned param_group_count(core::ParamKind kind) noexcept;
const char* param_group_name(core::ParamKind kind, unsigned group) noexcept;
unsigned param_group_fields(core::ParamKind kind, unsigned group, unsigned envelope, std::uint16_t* out, unsigned cap) noexcept;
// "Volume", "Panning", ... the names of the six envelope slots, for a page's header.
const char* envelope_name(unsigned envelope) noexcept;

// Reads / writes the storage of a field in its owner struct (`owner` points at the Instrument, TrackSlot, MasterSettings or the
// settings block's bytes). param_store accepts a finite value inside [min, max] and stores integers rounded to the nearest; false
// (nothing written) for anything else.
float param_load(const void* owner, const ParamField& f) noexcept;
bool param_store(void* owner, const ParamField& f, float value) noexcept;

struct Instrument;
// OS-INSTUI S5 (INIT): every parameter of a sample or MIDI instrument back to its descriptor's default, in place. Kept: the kind, the MIDI channel, the name, the sample (path, rate, start, loop and end
// points, the slices and their count: the Wave screen's data, not parameters) and the module fields. The sample type follows the play mode the default brings back (0, one shot), as play_mode_set keeps it.
void reset_instrument_params(Instrument& in) noexcept;

}  // namespace o3os::model
