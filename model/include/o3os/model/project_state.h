// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/model/limits.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace o3os::model {

// The project-level state that used to be runtime only (OS-004d, maintainer decision 2026-10-12: make it persistent at project level): the MIDI routing the settings table has no
// row for, the USB mode, profile and stem map, the mixer's routing, the Perform slots and the parameters of the SD apps. Plain values, no behaviour: the project keeps them
// (Project::midi() ... set_app_params()), the native format writes them as the chunks MIDI, USBS, MIXR, PERF and APPM (docs/formats/o3-project.md section 3), and the app and the
// platform copy them to and from the runtime objects that act on them (MidiLink's MidiConfig, the USB driver and the StemMap, MixBoard, PerformBank, ModuleUi).
//
// "Present" and defaults. A project that has never said anything about a part has no chunk for it and reads, in an older file, as this default. The model keeps one flag per
// part (Project::state_present()): set by the first set_*, written to the file as the chunk, read back as the flag. Mixer and Perform always have a value (the defaults below, which
// are the ones MixBoard and PerformBank start from, so a project that says nothing leaves them as they were at boot); MIDI and USB have none until the project says one, because
// their runtime values come from the DEVICE (the settings file's CLOCK OUT, MIDI OUT and OUT CHANNEL rows, the USB the unit last presented): a project with no MIDI or USB part
// leaves the device's as they are, one that has them applies them when it is loaded and every later change of the runtime is stored in it.
//
// OS-PERSIST2 adds four more parts of the same kind (the session state, docs/formats/o3-project.md section 3): the note-entry settings, the tracks' MUTE MODEs, the Perform scenes and the send
// effects' controls. Like Mixer and Perform they always have a value (the defaults below, which are what the runtime objects start from), and only a part that was set is written. Their
// chunks are read tolerantly (a value out of range is its default, counted: sanitise_*), not strictly: there is a safe default for each.
//
// The counts are the format's (o3project.py carries a copy that ctest compares); the constants that mirror a runtime object's (the MidiConfig, the USB profiles, the Perform FX
// table, the mixer's strips) are held equal to it by a static_assert where both are in sight (seq/midi_link.cpp, app/project_state.cpp), because the model must not include them.

// ---- MIDI: the fields of midi::MidiConfig that have no row in the device settings (or that the project may override)
inline constexpr std::uint32_t kMidiPorts = 2;          // DIN, USB (midi::kPortCount)
inline constexpr std::uint32_t kMidiCcSlots = 6;        // CC A..F
inline constexpr std::uint8_t kMidiPortMask = 3;        // bit 0 DIN, bit 1 USB
inline constexpr std::uint8_t kMidiCcUnassigned = 255;  // midi::kCcUnassigned
inline constexpr std::int32_t kMaxMidiOffsetDms = 1000; // +-100 ms, 0.1 ms units (midi::kMaxOffsetDms)
inline constexpr std::uint8_t kMidiFollowMax = 2;       // FOLLOW TIGHT | NORMAL | LOOSE

struct MidiState {
  std::uint8_t out_ports;       // CLOCK OUT: the ports that clock, Start, Continue, Stop and SPP go out on (bit 0 DIN, bit 1 USB)
  std::uint8_t note_ports;      // the ports the notes, CC and program changes of MIDI tracks go out on
  std::uint8_t notes_channel;   // 0..15: where a MIDI event without a channel goes
  std::uint8_t follow;          // the clock-follow loop's bandwidth: 0 tight, 1 normal, 2 loose
  std::uint8_t cc_number[kMidiCcSlots];      // CC A..F, kMidiCcUnassigned: sends nothing
  std::int16_t in_offset_dms[kMidiPorts];    // IN OFFSET per port, 0.1 ms
  std::int16_t out_offset_dms[kMidiPorts];   // OUT OFFSET per port, 0.1 ms
};

// ---- USB: the descriptor set, the stem profile and the slot map of the stems stream
inline constexpr std::uint32_t kStemSlots = 11;         // usb::kMaxChannels
inline constexpr std::uint8_t kUsbModeMax = 1;          // 0 STANDARD, 1 AUDIO (usb::Mode)
// UsbState::profile is the usb::Profile value: 0 P2, 1 P8, 2 P10 (the default), 3 P11, 5 P4, 6 P6 (OS-USBOUTS); usb::Profile::P4S (4) is not supported by the engine and is not stored.
// The USBS chunk's profile byte is only 0..3: a P4 or P6 project is written as P8 plus a channel-count byte (USBS 1.1, formats/native_state.h).
inline constexpr std::uint8_t kUsbProfileMax = 6;
inline constexpr std::uint8_t kUsbProfileP4S = 4;       // never valid
inline constexpr std::uint8_t kUsbProfileMaxChunk = 3;  // the highest profile byte of USBS 1.0
inline constexpr std::uint8_t kUsbProfileChannels[kUsbProfileMax + 1] = {2, 8, 10, 11, 0, 4, 6};  // usb::profile_channels

struct UsbState {
  std::uint8_t mode;
  std::uint8_t profile;
  std::uint8_t slots;     // = kUsbProfileChannels[profile]
  std::uint8_t reserved;  // 0
  std::uint16_t mask[kStemSlots];  // slot i sums the sources of bit set (audio/stem_map.h stem_src: T1..T12, Master L/R, Send A/B); slots past `slots` are 0
};

// ---- Mixer: the engine fields MixBoard holds (the model's own track and master parameters are stored in TRAK and PHDR)
inline constexpr std::uint32_t kMixStrips = 17;         // 16 tracks and the input strip (app::kMixStrips)
inline constexpr std::uint8_t kMixRouteMask = 63;       // MAIN, CUE, SNDA, SNDB, REC, USB (audio::route::kAll)
inline constexpr std::uint8_t kMixRouteUsb = 32;
inline constexpr std::uint8_t kMixDefaultTrackRoute = 1 | 32;  // MAIN + USB (graph.cpp init_defaults)
inline constexpr std::uint8_t kMixDefaultInputRoute = 1 | 2;   // MAIN + CUE
inline constexpr std::uint8_t kMixDefaultCue = 100;

struct MixState {
  std::uint8_t route[kMixStrips];  // audio::route bits of each strip; the input strip (16) has no USB bit
  std::uint8_t ret_route[2];       // where the SNDA / SNDB effect's return goes: bit 0 MAIN, bit 1 CUE
  std::uint8_t out_mode;           // 0 STEREO, 1 MAIN L / CUE R
  std::uint8_t cue_level;          // 0..100
  std::uint8_t in_level;           // 0..100
};

// ---- Perform: the twelve slots (seq/perform.h): the FX each holds, the row it is on and the values of its rows 1..3
inline constexpr std::uint32_t kPerfSlots = 12;
inline constexpr std::uint32_t kPerfStoredRows = 3;     // rows 1..3 (row 0 is "off" and has no value)
inline constexpr std::uint8_t kPerfRowMax = 3;
inline constexpr std::uint8_t kPerfFxMax = 21;          // seq::PerfFx::Overdrive: the last of the performance FX (ids 20 and 21 came with OS-PERFFX P1: the file says PERF / SCEN 1.1 when a slot holds one, formats/native.h kStateVersionFx; an older build reads them as none)
inline constexpr std::int32_t kPerfValueMax = 100;      // no FX of the table goes past +-100

struct PerfState {
  std::uint8_t fx[kPerfSlots];    // seq::PerfFx, 0 none
  std::uint8_t row[kPerfSlots];   // the selected row, 0..3 (0: off)
  std::int16_t value[kPerfSlots][kPerfStoredRows];  // rows 1..3
};

// OS-PRFDEFAULTS (new Community OS behaviour, docs/specs/OS-PERFFX.md section 9): the three values rows 1..3 of a slot get, by FX id (seq::PerfFx), mild to strong. A new project
// (init_perf_state) and a slot whose FX is changed in EDIT (PerformBank::change_fx) take them; a loaded project keeps what its file says. The FX with no consumer yet keep 0 (`---`).
inline constexpr std::int16_t kPerfDefaultValues[kPerfFxMax + 1u][kPerfStoredRows] = {
    {0, 0, 0},         // none
    {-25, -50, -100},  // VOLUME
    {-100, 100, -50},  // PANNING
    {-12, 12, 7},      // TUNE
    {-30, -60, -90},   // LOWPASS
    {30, 60, 90},      // HIPASS
    {-50, 30, 70},     // BANDPASS
    {30, 60, 100},     // DELAY
    {30, 60, 100},     // REVERB
    {25, 50, 75},      // SMP POS: a quarter, half, three quarters into the part (OS-PERFFX P7)
    {75, 50, 25},      // SMP END: the part cut to three quarters, half, a quarter
    {1, 1, 1},         // PLAYBACK: backward (the one live value; 0 ends the FX) (OS-PERF-REVERSE)
    {-8, -3, 3},       // VOL LFO: slow, medium, fast (OS-PERF-LFO)
    {-8, -3, 3},       // PAN LFO: slow, medium, fast (OS-PERF-LFO)
    {-8, -3, 3},       // TUNE LFO: slow, medium, fast (OS-PERF-LFO)
    {-8, -3, 3},       // FLT LFO: slow, medium, fast (OS-PERF-LFO)
    {-8, -3, 3},       // POS LFO: slow, medium, fast (OS-PERF-LFO)
    {11, 13, 15},      // STUTTER: 1/4, 1/8, 1/16
    {1, 2, 3},         // PLAYMODE: BACK, PING, RND
    {4, 3, 2},         // PAT LEN: 8, 4, 2 steps
    {4, 8, 12},        // BITDEPTH: 12, 8, 4 bits
    {20, 50, 80},      // OVERDRV
};

// ---- Apps: the parameter values of an SD app (app::ModuleUi), by the module's file stem and the ABI it was built with
// OS-APPUX S3: every shipped app stores its rows, so APPM 1.1 holds up to 12 apps (1.0 held 6: kAppParamSetsV10, still what the writer emits while no more are stored: docs/formats/o3-project.md). The
// least recently stored goes first when a 13th app, or more bytes than one chunk piece holds (kAppParamsBytesMax), come in.
inline constexpr std::uint32_t kMaxAppParamSets = 12;
inline constexpr std::uint32_t kAppParamSetsV10 = 6;
inline constexpr std::uint32_t kAppParamsBytesMax = 1015;  // the APPM payload: u16 version, u8 count, then per app 4 + name + 4 x values (formats/src/native_state.h asserts that it fits one writer piece)
inline constexpr std::uint32_t kMaxAppParams = 32;      // O3M_PARAMS_MAX
inline constexpr std::uint32_t kAppNameMax = 31;        // kModuleNameMax

struct AppParams {
  char name[kAppNameMax + 1];  // the module's file stem (a safe module name), NUL padded
  std::uint8_t abi_major;      // the ABI the module was built with: the values apply to the same major only
  std::uint8_t abi_minor;
  std::uint8_t count;          // values in use, 1..kMaxAppParams
  std::uint8_t reserved;       // 0
  float value[kMaxAppParams];  // finite; past `count` they are 0
};

// ---- Note entry (OS-PERSIST2): the root, the scale, the octave and SCALE LOCK of app/note_entry.h, and JUMP, the edit step of the Pattern screen (PatternEdit::edit_step)
inline constexpr std::uint8_t kNoteRootMax = 11;        // the pitch class C..B
inline constexpr std::uint8_t kNoteScaleMax = 11;       // app::kNoteScales - 1
inline constexpr std::uint8_t kNoteOctaveMax = 9;       // app::kEntryOctaves - 1
inline constexpr std::uint8_t kNoteJumpMax = 32;        // 0..32
inline constexpr std::uint8_t kNoteDefaultOctave = 3;   // with the C root, the C-3 that the pad board always started at
inline constexpr std::uint8_t kNoteDefaultJump = 1;

struct NoteState {
  std::uint8_t root;    // 0..kNoteRootMax
  std::uint8_t scale;   // 0..kNoteScaleMax
  std::uint8_t octave;  // 0..kNoteOctaveMax
  std::uint8_t lock;    // SCALE LOCK: 0 off, 1 on
  std::uint8_t jump;    // 0..kNoteJumpMax
};

// ---- Mute modes (OS-PERSIST2): each track's MUTE MODE (app/track_state.h MuteMode)
inline constexpr std::uint8_t kMuteModeMax = 2;         // 0 Immediate, 1 Bar, 2 Pattern

struct MuteState {
  std::uint8_t mode[kMaxTracks];
};

// ---- Perform scenes (OS-PERSIST2): three snapshots of the twelve slots, and LATCH (seq/perform.h PerformBank)
inline constexpr std::uint32_t kSceneCount = 3;         // seq::kPerfScenes

struct SceneState {
  std::uint8_t stored;    // 0 or 1; a scene that is not stored holds zeros
  std::uint8_t reserved;  // 0
  PerfState slots;        // the slots as they were when the scene was stored
};

struct ScenesState {
  std::uint8_t latch;     // 0 off, 1 on (our default)
  std::uint8_t reserved;  // 0
  SceneState scene[kSceneCount];
};

// ---- Send effects (OS-PERSIST2): the controls of the MASTER FX page, in the order of app/send_fx_rows.h kSendFxTable. A row is never renumbered; a new one is appended.
inline constexpr std::uint32_t kSendFxCount = 12;       // app::kSendFxRows
inline constexpr std::uint32_t kSendFxCountMax = 32;    // the most rows a chunk may carry
inline constexpr std::uint16_t kSendFxMin[kSendFxCount] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
inline constexpr std::uint16_t kSendFxMax[kSendFxCount] = {1, 1, 18, 3500, 70, 100, 1, 100, 100, 100, 100, 1};
inline constexpr std::uint16_t kSendFxDefault[kSendFxCount] = {0, 0, 0, 500, 50, 100, 0, 50, 50, 50, 80, 0};

struct SendFxState {
  std::uint16_t value[kSendFxCount];
};

static_assert(std::is_trivially_copyable<MidiState>::value && std::is_trivially_copyable<UsbState>::value && std::is_trivially_copyable<MixState>::value &&
                  std::is_trivially_copyable<PerfState>::value && std::is_trivially_copyable<AppParams>::value,
              "the project's state is plain bytes");
static_assert(sizeof(MidiState) == 18 && sizeof(UsbState) == 26 && sizeof(MixState) == 22 && sizeof(PerfState) == 96 && sizeof(AppParams) == 164,
              "the state structs have no padding (they are compared with memcmp)");
static_assert(std::is_trivially_copyable<NoteState>::value && std::is_trivially_copyable<MuteState>::value && std::is_trivially_copyable<SceneState>::value &&
                  std::is_trivially_copyable<ScenesState>::value && std::is_trivially_copyable<SendFxState>::value,
              "the session state is plain bytes");
static_assert(sizeof(NoteState) == 5 && sizeof(MuteState) == 16 && sizeof(SceneState) == 98 && sizeof(ScenesState) == 296 && sizeof(SendFxState) == 24,
              "the session state structs have no padding (they are compared with memcmp)");

// What the project holds before it has been told anything (the values a reset puts in; MIDI and USB are "absent" then, see above).
void init_midi_state(MidiState& out) noexcept;
void init_usb_state(UsbState& out) noexcept;
void init_mix_state(MixState& out) noexcept;
void init_perf_state(PerfState& out) noexcept;
void init_note_state(NoteState& out) noexcept;
void init_mute_state(MuteState& out) noexcept;
void init_scenes_state(ScenesState& out) noexcept;  // LATCH on, no scene stored
void init_send_fx_state(SendFxState& out) noexcept;

// nullptr when the value is one the project may hold (the format's rules), otherwise a static string saying what is wrong.
const char* validate_midi_state(const MidiState& s) noexcept;
const char* validate_usb_state(const UsbState& s) noexcept;
const char* validate_mix_state(const MixState& s) noexcept;
const char* validate_perf_state(const PerfState& s) noexcept;
const char* validate_app_params(const AppParams& s) noexcept;
const char* validate_note_state(const NoteState& s) noexcept;
const char* validate_mute_state(const MuteState& s) noexcept;
const char* validate_scenes_state(const ScenesState& s) noexcept;  // a scene that is not stored must hold zeros
const char* validate_send_fx_state(const SendFxState& s) noexcept;

// What a tolerant reader makes of a value it read (OS-PERSIST2): every field outside its range becomes its default, a scene that is not stored becomes zeros; the result passes validate_*().
// Returns how many fields were reset (the reader counts them: Report::settings_skipped).
unsigned sanitise_note_state(NoteState& s) noexcept;
unsigned sanitise_mute_state(MuteState& s) noexcept;
unsigned sanitise_scenes_state(ScenesState& s) noexcept;
unsigned sanitise_send_fx_state(SendFxState& s) noexcept;

inline bool operator==(const MidiState& a, const MidiState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const UsbState& a, const UsbState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const MixState& a, const MixState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const PerfState& a, const PerfState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const NoteState& a, const NoteState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const MuteState& a, const MuteState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const ScenesState& a, const ScenesState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator==(const SendFxState& a, const SendFxState& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool operator!=(const NoteState& a, const NoteState& b) noexcept { return !(a == b); }
inline bool operator!=(const MuteState& a, const MuteState& b) noexcept { return !(a == b); }
inline bool operator!=(const ScenesState& a, const ScenesState& b) noexcept { return !(a == b); }
inline bool operator!=(const SendFxState& a, const SendFxState& b) noexcept { return !(a == b); }
inline bool operator!=(const MidiState& a, const MidiState& b) noexcept { return !(a == b); }
inline bool operator!=(const UsbState& a, const UsbState& b) noexcept { return !(a == b); }
inline bool operator!=(const MixState& a, const MixState& b) noexcept { return !(a == b); }
inline bool operator!=(const PerfState& a, const PerfState& b) noexcept { return !(a == b); }

}  // namespace o3os::model
