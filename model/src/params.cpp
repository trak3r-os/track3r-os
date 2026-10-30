// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/params.h>

#include <o3os/model/limits.h>
#include <o3os/model/project.h>
#include <o3os/model/settings.h>

#include <cstddef>
#include <cstring>

namespace o3os::model {

namespace {

using core::ParamCurve;
using core::ParamDesc;
using core::ParamKnot;
using core::ParamUnit;

constexpr std::uint8_t kL = core::kParamLearnable;
constexpr std::uint8_t kM = core::kParamModulatable;
constexpr std::uint8_t kS = core::kParamSmooth;
constexpr std::uint8_t kA = core::kParamApply;

constexpr const char* const kOffOn[] = {"Off", "On"};
constexpr const char* const kFilterTypes[] = {"Low-pass", "High-pass", "Band-pass"};            // the filter mode labels
constexpr const char* const kPlayEngines[] = {"Auto", "Modern", "Amiga", "12-bit", "8-bit"};   // kInstEngine* (INST 1.2)
// OS-INSTUI S3, the source page. The play modes are in the instrument file's order (the labels shortened to the
// value column's width); the grain's shapes and loop types are (Square / Triangle / Gauss, Forward / Backward / PingPong).
constexpr const char* const kPlayModes[] = {"One shot", "Loop fwd", "Loop bwd", "Ping-pong", "Slice", "Beat slice", "Wavetable", "Granular"};
constexpr const char* const kGrainShapes[] = {"Square", "Triangle", "Gauss"};
constexpr const char* const kGrainLoops[] = {"Forward", "Backward", "PingPong"};
// OS-INSTUI S6, the MOD page: the SOURCE words, the LFO shape labels and the LFO speed labels (29, the tempo-sync rates;
// the amp slot takes the last 24, from the sixth on).
constexpr const char* const kModSources[] = {"Off", "Envelope", "LFO"};
constexpr const char* const kLfoShapes[] = {"Rev Saw", "Saw", "Triangle", "Square", "Random", "Sine"};
constexpr const char* const kLfoSpeeds[] = {"128 steps", "96 steps", "64 steps", "48 steps", "32 steps", "24 steps", "16 steps", "12 steps", "8 steps", "6 steps", "4 steps", "3 steps", "2 steps", "3/2 step", "1 step",
                                            "3/4 step", "1/2 step", "3/8 step", "1/3 step", "1/4 step", "3/16 step", "1/6 step", "1/8 step", "1/12 step", "1/16 step", "1/24 step", "1/32 step", "1/48 step", "1/64 step"};
constexpr unsigned kAmpSpeedBase = instrument_param::kOtherLfoSpeeds - instrument_param::kAmpLfoSpeeds;
constexpr const char* const kBrightness[] = {"Low", "Medium", "High"};                            // config menu label
constexpr const char* const kClockIn[] = {"Internal", "USB", "MIDI In jack"};                    // config menu label
constexpr const char* const kLimiterModes[] = {"Subtle", "Extreme"};                             // config menu label
constexpr const char* const kSendsModes[] = {"Pre-fader", "Post-fader"};                         // PAR-024
constexpr const char* const kNumbering[] = {"From 0", "From 1", "Hex"};                          // PAR-120; the names are ours
constexpr const char* const kMiddleC[] = {"C-3", "C-4", "C-5", "C-6"};                           // PAR-084
// OS-011a (new Community OS settings: the clock driver and the MIDI router read them, they do not exist yet; the defaults keep today's behaviour: no clock out, both ports, every channel).
constexpr const char* const kClockModes[] = {"Off", "Master", "Slave"};
constexpr const char* const kMidiPorts[] = {"DIN", "USB", "DIN + USB"};
constexpr const char* const kInChannels[] = {"All", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};
constexpr const char* const kThemes[] = {"Amiga", "Black", "White"};                              // ui/theme.h ThemeId
constexpr const char* const kRecPoly[] = {"Off", "Spill"};                                         // OS-LIVEREC
constexpr const char* const kRecQuantize[] = {"Nearest step", "Playing step"};                   // OS-LIVEREC
constexpr const char* const kMetroModes[] = {"Off", "Cue", "Main"};                              // OS-METRO
constexpr const char* const kCountIns[] = {"Off", "1 bar", "2 bars"};                            // OS-METRO
constexpr const char* const kRecVelocity[] = {"Off", "External"};                                  // OS-RECMICRO
constexpr const char* const kPatSwitches[] = {"Step", "Pattern end"};                              // E-PAT-35
constexpr const char* const kMidiOuts[] = {"Config", "DIN", "USB", "DIN + USB"};                   // OS-INSTUI S8b: the instrument's MIDI port (kMidiOut*); Config = the device's MIDI OUT

// The envelope time scale: 100 steps of 10 ms up to a second, then 90 steps
// of 100 ms up to ten seconds. One curve serves the bar and the encoder: the knot at 100/190 makes every detent one 1/190 of it (the second is at 52.6 %).
constexpr ParamKnot kEnvTimeKnots[] = {{0.0f, 0.0f}, {100.0f / 190.0f, 1000.0f}, {1.0f, 10000.0f}};

constexpr ParamDesc number(const char* name, const char* unit_text, ParamUnit unit, float min, float max, float def,
                           unsigned detents, unsigned coarse, std::uint8_t flags, unsigned group) noexcept {
  return ParamDesc{name, unit_text, nullptr, nullptr, min, max, def, static_cast<std::uint16_t>(detents),
                   static_cast<std::uint8_t>(coarse), 0, unit, ParamCurve::Linear, flags, static_cast<std::uint8_t>(group)};
}

constexpr ParamDesc choice(const char* name, const char* const* names, unsigned count, unsigned def, std::uint8_t flags,
                           unsigned group) noexcept {
  return ParamDesc{name, "", names, nullptr, 0.0f, static_cast<float>(count - 1u), static_cast<float>(def),
                   static_cast<std::uint16_t>(count - 1u), 1, static_cast<std::uint8_t>(count), ParamUnit::Enum, ParamCurve::Linear, flags,
                   static_cast<std::uint8_t>(group)};
}

constexpr ParamDesc env_time(const char* name, float def, unsigned group) noexcept {
  return ParamDesc{name, "s", nullptr, kEnvTimeKnots, 0.0f, 10000.0f, def, 190, 10, 3, ParamUnit::Seconds, ParamCurve::Table, kL,
                   static_cast<std::uint8_t>(group)};
}

// A row of a table: the descriptor, then where the value is stored (see ParamField).
struct Row {
  ParamDesc desc;
  std::uint16_t offset;
  ParamStore store;
  std::uint8_t mask;
};

constexpr Row at(const ParamDesc& d, std::size_t offset, ParamStore store, std::uint8_t mask = 0) noexcept {
  return Row{d, static_cast<std::uint16_t>(offset), store, mask};
}

#define O3_SAMPLER(member) (offsetof(Instrument, sampler) + offsetof(SamplerParams, member))

// ---- kind Instrument: the defaults are this OS's own, which is also what init_instrument
// makes: volume 50, panning 50, cutoff 1.0 (filter off), bit depth 16, every envelope with amount 1.0, sustain 1.0, release 1000 ms; the volume
// envelope on with a 10 ms attack, the other five off with a 3000 ms attack.

constexpr Row kInstrument[instrument_param::kBase] = {
    at(number("VOLUME", "%", ParamUnit::Int, 0, 100, 50, 100, 10, kL | kM | kS, instrument_param::GroupVoice), O3_SAMPLER(volume), ParamStore::U8),
    at(number("PANNING", "", ParamUnit::Pan, 0, 100, 50, 100, 10, kL | kM | kS, instrument_param::GroupVoice), O3_SAMPLER(panning), ParamStore::U8),
    at(number("TUNE", "st", ParamUnit::Int, -24, 24, 0, 48, 12, kL, instrument_param::GroupVoice), O3_SAMPLER(tune), ParamStore::I8),
    at(number("FINE TUNE", "ct", ParamUnit::Int, -100, 100, 0, 200, 10, kL | kM, instrument_param::GroupVoice), O3_SAMPLER(fine_tune), ParamStore::I8),
    at(number("GLIDE", "s", ParamUnit::Seconds, 0, 15000, 0, 150, 10, kL, instrument_param::GroupVoice), O3_SAMPLER(glide), ParamStore::U16),
    at(choice("FILTER", kOffOn, 2, 0, kL, instrument_param::GroupFilter), O3_SAMPLER(filter_enable), ParamStore::U8),
    at(choice("FILTER TYPE", kFilterTypes, 3, 0, kL, instrument_param::GroupFilter), O3_SAMPLER(filter_type), ParamStore::U8),
    at(number("CUTOFF", "%", ParamUnit::NormPct, 0, 1, 1, 100, 10, kL | kM | kS, instrument_param::GroupFilter), O3_SAMPLER(cutoff), ParamStore::F32),
    at(number("RESONANCE", "%", ParamUnit::NormPct, 0, 4.3f, 0, 100, 10, kL, instrument_param::GroupFilter), O3_SAMPLER(resonance), ParamStore::F32),
    at(number("DELAY SEND", "%", ParamUnit::Int, 0, 100, 0, 100, 10, kL, instrument_param::GroupSends), O3_SAMPLER(delay_send), ParamStore::U8),
    at(number("REVERB SEND", "%", ParamUnit::Int, 0, 100, 0, 100, 10, kL, instrument_param::GroupSends), O3_SAMPLER(reverb_send), ParamStore::U8),
    at(number("OVERDRIVE", "%", ParamUnit::Int, 0, 100, 0, 100, 10, kL, instrument_param::GroupSends), O3_SAMPLER(overdrive), ParamStore::U8),
    at(number("BIT DEPTH", "bit", ParamUnit::Int, 4, 16, 16, 12, 4, kL, instrument_param::GroupSends), O3_SAMPLER(bit_depth), ParamStore::U8),
    at(choice("PLAY ENGINE", kPlayEngines, 5, kInstEngineAuto, 0, instrument_param::GroupSource), offsetof(Instrument, engine), ParamStore::U8),  // OS-FU5; OS-INSTUI O5: on the source page since S3 (it was on VOICE)
};

// OS-INSTUI S3: the rows of the source page, in id order from instrument_param::kSourceFirst. Ranges: the window size 32..2048 (exponents 5 / 11, a power
// of two; 0 here is "not set", which the engine plays as 2048), the grain length 44..44100, a position 0..65535, shape and loop type
// 3 grain shapes and 3 loop types; 0 is "not set" for the window size and the grain length (init_instrument zeroes them; the engine plays 2048 and 441), slices 0..48 and the selected slice 0..47. The wavetable window is a u32 in the struct whose low half is edited (a window count past 65535 would
// need a 32 sample window on a 49 second sample): little-endian on the host and the MK66, which the assert below holds.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "WT POS stores the low half of a u32 as a u16");
constexpr Row kSource[instrument_param::kSourceRows] = {
    at(choice("PLAY MODE", kPlayModes, 8, 0, 0, instrument_param::GroupSource), O3_SAMPLER(play_mode), ParamStore::U8),  // (not learnable, like PLAY ENGINE: the rules live in the action)
    at(number("WT POS", "", ParamUnit::Int, 0, 65535, 0, 65535, 16, 0, instrument_param::GroupSource), O3_SAMPLER(wavetable_window), ParamStore::U16),
    at(number("WT SIZE", "smp", ParamUnit::Int, 0, 2048, 0, 2048, 10, 0, instrument_param::GroupSource), O3_SAMPLER(wavetable_window_size), ParamStore::U16),  // (by counters, in the action)
    at(number("GRAIN POS", "", ParamUnit::Int, 0, 65535, 0, 1000, 10, kL, instrument_param::GroupSource), O3_SAMPLER(grain_position), ParamStore::U16),  // (the page shows it in seconds)
    at(number("GRAIN LENGTH", "smp", ParamUnit::Int, 0, 44100, 0, 4000, 10, kL, instrument_param::GroupSource), O3_SAMPLER(grain_length), ParamStore::U16),  // (0: not set, the engine plays 441; the edit holds 44 .. min(length, 44100))
    at(choice("GRAIN SHAPE", kGrainShapes, 3, 0, kL, instrument_param::GroupSource), O3_SAMPLER(grain_shape), ParamStore::U8),
    at(choice("GRAIN LOOP", kGrainLoops, 3, 0, kL, instrument_param::GroupSource), O3_SAMPLER(grain_type), ParamStore::U8),
    at(number("SLICES", "", ParamUnit::Int, 0, 48, 0, 48, 4, 0, instrument_param::GroupSource), O3_SAMPLER(slice_count), ParamStore::U8),  // (a readout: the slices are made on Wave)
    at(number("SLICE", "", ParamUnit::Int, 0, 47, 0, 47, 4, 0, instrument_param::GroupSource), O3_SAMPLER(selected_slice), ParamStore::U8),
};

#undef O3_SAMPLER

// OS-INSTUI S8b + S9: the stored fields of a MIDI instrument (INST 1.3, instrument.h) and the sample instrument's VEL>, one byte each. Ranges, defaults and the meaning of every value are instrument.h's
// (kMidi*): the program and the CC slots are shown through their own units (core::ParamUnit::Program, CcSlot). The page numbers are the MIDI instrument's (instrument_param::GroupMidi, GroupCc); VEL> is on VOICE.
constexpr ParamDesc coded(const char* name, float max, float def, ParamUnit unit, unsigned group) noexcept {
  return ParamDesc{name, "", nullptr, nullptr, 0.0f, max, def, static_cast<std::uint16_t>(max), 10, 0, unit, ParamCurve::Linear, kL, static_cast<std::uint8_t>(group)};
}

constexpr Row kMidi[instrument_param::kMidiRows] = {
    at(number("VELOCITY", "", ParamUnit::Int, 1, 127, kMidiVelocityDefault, 126, 10, kL, instrument_param::GroupMidi), offsetof(Instrument, midi_velocity), ParamStore::U8),
    at(coded("PROGRAM", kMidiProgramMax, kMidiProgramNone, ParamUnit::Program, instrument_param::GroupMidi), offsetof(Instrument, midi_program), ParamStore::U8),
    at(choice("OUT", kMidiOuts, 4, kMidiOutConfig, kL, instrument_param::GroupMidi), offsetof(Instrument, midi_out), ParamStore::U8),
    at(number("PB RANGE", "st", ParamUnit::Int, 0, kMidiPbRangeMax, kMidiPbRangeDefault, kMidiPbRangeMax, 4, kL, instrument_param::GroupMidi), offsetof(Instrument, midi_pb_range), ParamStore::U8),
    at(number("OFFSET", "ms", ParamUnit::Int, 0, kMidiOffsetMaxMs, 0, kMidiOffsetMaxMs, 10, kL, instrument_param::GroupMidi), offsetof(Instrument, midi_offset), ParamStore::U8),
    at(coded("CC A", kMidiCcMax, kMidiCcConfig, ParamUnit::CcSlot, instrument_param::GroupCc), offsetof(Instrument, midi_cc) + 0, ParamStore::U8),
    at(coded("CC B", kMidiCcMax, kMidiCcConfig, ParamUnit::CcSlot, instrument_param::GroupCc), offsetof(Instrument, midi_cc) + 1, ParamStore::U8),
    at(coded("CC C", kMidiCcMax, kMidiCcConfig, ParamUnit::CcSlot, instrument_param::GroupCc), offsetof(Instrument, midi_cc) + 2, ParamStore::U8),
    at(coded("CC D", kMidiCcMax, kMidiCcConfig, ParamUnit::CcSlot, instrument_param::GroupCc), offsetof(Instrument, midi_cc) + 3, ParamStore::U8),
    at(coded("CC E", kMidiCcMax, kMidiCcConfig, ParamUnit::CcSlot, instrument_param::GroupCc), offsetof(Instrument, midi_cc) + 4, ParamStore::U8),
    at(coded("CC F", kMidiCcMax, kMidiCcConfig, ParamUnit::CcSlot, instrument_param::GroupCc), offsetof(Instrument, midi_cc) + 5, ParamStore::U8),
    at(number("VEL>", "%", ParamUnit::Int, 0, 100, kVelSensDefault, 100, 10, kL, instrument_param::GroupVoice), offsetof(Instrument, vel_sens), ParamStore::U8),
};
static_assert(instrument_param::kMidiRows == 6 + kMidiCcSlotsPerInstrument, "the MIDI rows are VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET, CC A..F, VEL>");

// One row per field of an envelope; the offset is that of envelope 0, the others add e * sizeof(Envelope).
#define O3_ENVELOPE(member) (offsetof(Instrument, sampler) + offsetof(SamplerParams, envelope) + offsetof(Envelope, member))

constexpr Row kEnvelope[instrument_param::kEnvelopeFields] = {
    at(choice("ENABLE", kOffOn, 2, 0, kL, instrument_param::GroupEnvelope), O3_ENVELOPE(enable), ParamStore::U8),
    at(number("AMOUNT", "%", ParamUnit::NormPct, 0, 1, 1, 100, 10, kL, instrument_param::GroupEnvelope), O3_ENVELOPE(amount), ParamStore::F32),
    at(env_time("ATTACK", 3000, instrument_param::GroupEnvelope), O3_ENVELOPE(attack), ParamStore::U16),
    at(env_time("DECAY", 0, instrument_param::GroupEnvelope), O3_ENVELOPE(decay), ParamStore::U16),
    at(number("SUSTAIN", "%", ParamUnit::NormPct, 0, 1, 1, 100, 10, kL, instrument_param::GroupEnvelope), O3_ENVELOPE(sustain), ParamStore::F32),
    at(env_time("RELEASE", 1000, instrument_param::GroupEnvelope), O3_ENVELOPE(release), ParamStore::U16),
};

// The volume envelope (0) differs from the five others in two defaults: it is enabled and attacks in 10 ms.
constexpr Row kVolumeEnvelopeEnable = at(choice("ENABLE", kOffOn, 2, 1, kL, instrument_param::GroupEnvelope), O3_ENVELOPE(enable), ParamStore::U8);
constexpr Row kVolumeEnvelopeAttack = at(env_time("ATTACK", 10, instrument_param::GroupEnvelope), O3_ENVELOPE(attack), ParamStore::U16);

// OS-INSTUI S6: SOURCE, a state of two bytes: `loop` is the field's byte and `enable` the next. The volume envelope's default is Envelope, the five others' Off.
static_assert(offsetof(Envelope, enable) == offsetof(Envelope, loop) + 1u, "SOURCE reads loop and enable as two neighbouring bytes");
constexpr Row kModSource = at(choice("SOURCE", kModSources, 3, instrument_param::SourceOff, kL, instrument_param::GroupEnvelope), O3_ENVELOPE(loop), ParamStore::EnvState);
constexpr Row kVolumeModSource = at(choice("SOURCE", kModSources, 3, instrument_param::SourceEnvelope, kL, instrument_param::GroupEnvelope), O3_ENVELOPE(loop), ParamStore::EnvState);

#undef O3_ENVELOPE

// The LFO of slot e (SamplerParams::lfo[e]); the offsets are those of lfo[0], the others add e * sizeof(Lfo). Ranges follow the instrument file, defaults are ours: shape 0..5 (5 Sine, OS-LFOSINE), default Triangle;
// speed 0..28, the amp slot's 0..23, default 0; amount 0..1 in steps of 0.01, default 0.5
// (init_instrument stores the same). The names carry "LFO": a destination's page lists its own AMOUNT beside them.
#define O3_LFO(member) (offsetof(Instrument, sampler) + offsetof(SamplerParams, lfo) + offsetof(Lfo, member))

constexpr Row kLfo[instrument_param::kModFields - 1u] = {
    at(choice("LFO SHAPE", kLfoShapes, 6, 2, kL, instrument_param::GroupEnvelope), O3_LFO(shape), ParamStore::U8),
    at(choice("LFO SPEED", kLfoSpeeds, instrument_param::kOtherLfoSpeeds, 0, kL, instrument_param::GroupEnvelope), O3_LFO(speed), ParamStore::U8),
    at(number("LFO AMOUNT", "%", ParamUnit::NormPct, 0, 1, 0.5f, 100, 10, kL, instrument_param::GroupEnvelope), O3_LFO(amount), ParamStore::F32),
};
constexpr Row kAmpLfoSpeed = at(choice("LFO SPEED", kLfoSpeeds + kAmpSpeedBase, instrument_param::kAmpLfoSpeeds, 0, kL, instrument_param::GroupEnvelope), O3_LFO(speed), ParamStore::U8);

#undef O3_LFO

constexpr const char* const kEnvelopeNames[instrument_param::kEnvelopes] = {
    "Volume", "Panning", "Cutoff", "WT Position", "Gran Position", "Finetune"};  // the two long labels shortened: "ENVELOPE 4/6 " + the name must clear the "ENV 3/4" tag of the Params header (16 chars at most)

// ---- kind Track (the mixer part of a slot) and Master -----------------------------------------------------------------

constexpr float kVolume = static_cast<float>(kDefaultVolume);
constexpr float kPan = static_cast<float>(kDefaultPan);

constexpr Row kTrack[track_param::kCount] = {
    at(number("VOLUME", "%", ParamUnit::Int, 0, 100, kVolume, 100, 10, kL | kS, 0), offsetof(TrackSlot, volume), ParamStore::U8),
    at(number("PAN", "", ParamUnit::Pan, 0, 100, kPan, 100, 10, kL | kS, 0), offsetof(TrackSlot, pan), ParamStore::U8),
    at(choice("MUTE", kOffOn, 2, 0, kL, 0), offsetof(TrackSlot, flags), ParamStore::U8, kTrackMute),
    at(choice("SOLO", kOffOn, 2, 0, kL, 0), offsetof(TrackSlot, flags), ParamStore::U8, kTrackSolo),
    at(number("BUS", "", ParamUnit::Int, 0, static_cast<float>(kMaxOutputBus), 0, kMaxOutputBus, 4, kL, 0), offsetof(TrackSlot, bus), ParamStore::U8),
};

constexpr Row kMaster[master_param::kCount] = {
    at(number("VOLUME", "%", ParamUnit::Int, 0, 100, kVolume, 100, 10, kL | kS, 0), offsetof(MasterSettings, volume), ParamStore::U8),
    at(number("DRY", "%", ParamUnit::Int, 0, 100, kVolume, 100, 10, kL | kS, 0), offsetof(MasterSettings, dry_volume), ParamStore::U8),
    at(number("REVERB", "%", ParamUnit::Int, 0, 100, kVolume, 100, 10, kL | kS, 0), offsetof(MasterSettings, reverb_volume), ParamStore::U8),
    at(number("DELAY", "%", ParamUnit::Int, 0, 100, kVolume, 100, 10, kL | kS, 0), offsetof(MasterSettings, delay_volume), ParamStore::U8),
    at(choice("DRY MUTE", kOffOn, 2, 0, kL, 0), offsetof(MasterSettings, flags), ParamStore::U8, kMasterDryMute),
    at(choice("REVERB MUTE", kOffOn, 2, 0, kL, 0), offsetof(MasterSettings, flags), ParamStore::U8, kMasterReverbMute),
    at(choice("DELAY MUTE", kOffOn, 2, 0, kL, 0), offsetof(MasterSettings, flags), ParamStore::U8, kMasterDelayMute),
    at(choice("LIMITER", kOffOn, 2, 0, kL, 0), offsetof(MasterSettings, flags), ParamStore::U8, kMasterLimiter),
};

// ---- kinds Project and Device: offsets are bytes of the settings block's values (Settings::kValueBytes) ---------------

constexpr Row kProject[project_setting::kCount] = {
    at(choice("ANTI-ALIAS", kOffOn, 2, 1, kL, 0), 0, ParamStore::U8),       // our default: on
    at(choice("LIMITER", kLimiterModes, 2, 0, kL, 0), 1, ParamStore::U8),   // our default: Subtle
    at(choice("SENDS", kSendsModes, 2, 0, kL, 0), 2, ParamStore::U8),       // the default: sends are pre-fader (triage section 5)
};

// The pages of the Config screen: group 0 MIDI (and the clock), group 1 DISPLAY (and how numbers and notes are shown), group 2 USB (OS-USBRET), group 3 PATTERN (OS-LIVEREC; and, since OS-METRO, the metronome's three rows).
constexpr unsigned kGroupMidi = 0;
constexpr unsigned kGroupDisplay = 1;
constexpr unsigned kGroupUsb = 2;
constexpr unsigned kGroupPattern = 3;

constexpr Row kDevice[device_setting::kCount] = {
    at(choice("DISPLAY", kBrightness, 3, 2, kL, kGroupDisplay), 0, ParamStore::U8),                           // our default: 2 (High)
    at(choice("PAD LIGHTS", kBrightness, 3, 2, kL, kGroupDisplay), 1, ParamStore::U8),                        // our default: 2 (High)
    at(number("METRO VOL", "%", ParamUnit::Int, 0, 100, 50, 100, 10, kL, kGroupPattern), 2, ParamStore::U8),  // our default: 50. OS-METRO: on the PATTERN page since the metronome exists (it was the MIDI page's dead row)
    at(choice("CLOCK IN", kClockIn, 3, 0, kL, kGroupMidi), 3, ParamStore::U8),                                // our default: Internal
    at(choice("NUMBERING", kNumbering, 3, 1, kL, kGroupDisplay), 4, ParamStore::U8),                          // from 1: our default
    at(choice("MIDDLE C", kMiddleC, 4, 2, kL, kGroupDisplay), 5, ParamStore::U8),                             // OS-CFGWIRE: C-5, the name of MIDI note 60 when note names start at C0 (the names are unchanged by default)
    at(number("CLOCK SYNC", "pls", ParamUnit::Int, -6, 6, 0, 12, 3, kL, kGroupMidi), 6, ParamStore::I8),      // PAR-119: -6..+6 pulses
    at(choice("CLOCK MODE", kClockModes, 3, 0, kL, kGroupMidi), 7, ParamStore::U8),                           // OS-011a: Off, Master, Slave
    at(choice("MIDI IN", kMidiPorts, 3, 2, kL, kGroupMidi), 8, ParamStore::U8),                               // OS-011a: DIN + USB
    at(choice("MIDI OUT", kMidiPorts, 3, 2, kL, kGroupMidi), 9, ParamStore::U8),                              // OS-011a: DIN + USB
    at(choice("IN CHANNEL", kInChannels, 17, 0, kL, kGroupMidi), 10, ParamStore::U8),                         // OS-011a: All (0), or 1..16
    at(number("OUT CHANNEL", "", ParamUnit::Int, 1, 16, 1, 15, 4, kL, kGroupMidi), 11, ParamStore::U8),       // OS-011a: 1..16
    at(choice("THEME", kThemes, 3, 0, kL, kGroupDisplay), 12, ParamStore::U8),                                // OS-011a: Amiga (ui::ThemeId)
    at(choice("USB RETURN", kOffOn, 2, 1, kA, kGroupUsb), 13, ParamStore::U8),                                // OS-USBRET: On, the descriptors before it. Not learnable, press to apply: a change re-enumerates USB
    at(choice("REC POLY", kRecPoly, 2, 0, kL, kGroupPattern), 14, ParamStore::U8),                            // OS-LIVEREC: Off, the current track only
    at(choice("REC QUANTIZE", kRecQuantize, 2, 0, kL, kGroupPattern), 15, ParamStore::U8),                    // OS-LIVEREC: Nearest step
    at(number("RETURN LEVEL", "%", ParamUnit::Int, 0, 100, 100, 100, 10, kL | kS, kGroupUsb), 16, ParamStore::U8),  // OS-006e: the USB return to the outputs, unity (the mixer's law)
    at(choice("USB AUDIO", kOffOn, 2, 1, kA, kGroupUsb), 17, ParamStore::U8),                                 // OS-USBAUDIO: On, the unit enumerates as the audio device. Not learnable, press to apply (the Config page shows it first)
    at(choice("METRO", kMetroModes, 3, 1, kL, kGroupPattern), 18, ParamStore::U8),                            // OS-METRO: Cue (Main while OUT MODE is Stereo)
    at(choice("COUNT-IN", kCountIns, 3, 0, kL, kGroupPattern), 19, ParamStore::U8),                           // OS-METRO: Off
    at(choice("REC MICRO", kOffOn, 2, 0, kL, kGroupPattern), 20, ParamStore::U8),                             // OS-RECMICRO: Off
    at(choice("REC VELOCITY", kRecVelocity, 2, 0, kL, kGroupPattern), 21, ParamStore::U8),                    // OS-RECMICRO: Off
    at(number("PAD OFFSET", "", ParamUnit::Int, 0, 12, 12, 12, 3, kL, kGroupPattern), 22, ParamStore::U8),   // OS-PADOFFSET: 12, every row follows the one below it (the layout before the row)
    at(choice("TRANSP IN", kOffOn, 2, 1, kL, kGroupMidi), 23, ParamStore::U8),                                // E-CFG-03: TRANSPORT IN (the caption holds 12 characters): On, Start / Stop / SPP from the followed port act (today's behaviour)
    at(choice("TRANSP OUT", kOffOn, 2, 1, kL, kGroupMidi), 24, ParamStore::U8),                               // E-CFG-03: TRANSPORT OUT: On, Start / Stop / SPP are sent (today's behaviour)
    at(choice("PAT SWITCH", kPatSwitches, 2, 0, kL, kGroupPattern), 25, ParamStore::U8),                      // E-PAT-35: Step (today's behaviour: a pick in the held PATTERN chooser switches at the next step)
};

static_assert(Settings::kValueBytes >= device_setting::kCount && Settings::kValueBytes >= project_setting::kCount &&
                  Settings::kMaxFields >= device_setting::kCount && Settings::kMaxFields >= project_setting::kCount,
              "the settings block holds the fields of both scopes");

static_assert(sizeof kPlayEngines / sizeof kPlayEngines[0] == kInstEngineMax + 1u, "PLAY ENGINE lists every kInstEngine*");

constexpr unsigned kEnvelopeRows = instrument_param::kEnvelopeFields;
constexpr unsigned kModSlotBase = instrument_param::kBase + instrument_param::kSourceRows + instrument_param::kEnvelopes * instrument_param::kEnvelopeFields;  // OS-INSTUI S6: the first slot of the MOD rows
constexpr unsigned kMidiSlotBase = kModSlotBase + instrument_param::kEnvelopes * instrument_param::kModFields;  // OS-INSTUI S8b: the first slot of the MIDI rows

// The one decode of an id: its table row, and for an envelope field how many bytes its envelope lies past envelope 0 (0 otherwise).
// nullptr when the kind, the index or the field does not exist.
const Row* row_of(core::ParamId id, std::size_t& extra) noexcept {
  const unsigned index = core::param_index(id);
  const unsigned field = core::param_field(id);
  extra = 0;
  switch (core::param_kind(id)) {
    case core::ParamKind::Instrument:
      if (index >= kMaxInstruments) {
        return nullptr;
      }
      if (field < instrument_param::kBase) {
        return &kInstrument[field];
      }
      if (instrument_param::is_source_field(field)) {
        return &kSource[field - instrument_param::kSourceFirst];
      }
      if (instrument_param::is_midi_row(field)) {  // OS-INSTUI S8b + S9
        return &kMidi[field - instrument_param::kMidiFirst];
      }
      if (instrument_param::is_mod_field(field)) {  // OS-INSTUI S6
        const unsigned rel = field - instrument_param::kModFirst;
        const unsigned e = rel / instrument_param::kModStride;
        const unsigned f = rel % instrument_param::kModStride;
        if (f == instrument_param::ModSource) {
          extra = e * sizeof(Envelope);
          return e == 0 ? &kVolumeModSource : &kModSource;
        }
        extra = e * sizeof(Lfo);
        return f == instrument_param::LfoSpeed && e == 0 ? &kAmpLfoSpeed : &kLfo[f - 1u];
      }
      if (field >= instrument_param::kEnvelopeFirst) {
        const unsigned rel = field - instrument_param::kEnvelopeFirst;
        const unsigned e = rel / instrument_param::kEnvelopeStride;
        const unsigned f = rel % instrument_param::kEnvelopeStride;
        if (e < instrument_param::kEnvelopes && f < kEnvelopeRows) {
          extra = e * sizeof(Envelope);
          if (e == 0 && f == instrument_param::EnvEnable) {
            return &kVolumeEnvelopeEnable;
          }
          return e == 0 && f == instrument_param::EnvAttack ? &kVolumeEnvelopeAttack : &kEnvelope[f];
        }
      }
      return nullptr;
    case core::ParamKind::Track:
      return index < kMaxTracks && field < track_param::kCount ? &kTrack[field] : nullptr;
    case core::ParamKind::Master:
      return index == 0 && field < master_param::kCount ? &kMaster[field] : nullptr;
    case core::ParamKind::Project:
      return index == 0 && field < project_setting::kCount ? &kProject[field] : nullptr;
    case core::ParamKind::Device:
      return index == 0 && field < device_setting::kCount ? &kDevice[field] : nullptr;
    case core::ParamKind::None:
    default:
      return nullptr;
  }
}

}  // namespace

bool find_param(core::ParamId id, ParamField& out) noexcept {
  std::size_t extra;
  const Row* r = row_of(id, extra);
  if (r == nullptr) {
    return false;
  }
  out = ParamField{&r->desc, static_cast<std::uint16_t>(r->offset + extra), r->store, r->mask};
  return true;
}

const core::ParamDesc* param_desc(core::ParamId id) noexcept {
  std::size_t extra;
  const Row* r = row_of(id, extra);
  return r != nullptr ? &r->desc : nullptr;
}

const core::ParamDesc& midi_channel_desc() noexcept {
  static constexpr ParamDesc kDesc = number("CHANNEL", "", ParamUnit::Int, 1, 16, 1, 15, 4, 0, instrument_param::GroupVoice);  // (the group is not used: the MIDI page is the screen's own)
  return kDesc;
}

const core::ParamDesc& play_stereo_desc() noexcept {
  static constexpr const char* const kStereoChoices[] = {"MONO", "STEREO"};  // Instrument::stereo (INST 1.5)
  static constexpr ParamDesc kDesc = choice("STEREO", kStereoChoices, 2, 0, 0, instrument_param::GroupSource);
  return kDesc;
}

bool instrument_field_app_writable(unsigned field) noexcept {
  namespace ip = instrument_param;
  if (field < ip::kBase) {
    return field != ip::PlayEngine;
  }
  if (ip::is_mod_field(field)) {
    return (field - ip::kModFirst) % ip::kModStride != ip::ModSource;
  }
  if (field >= ip::kEnvelopeFirst && field < ip::kEnvelopeFirst + ip::kEnvelopes * ip::kEnvelopeStride) {
    const unsigned f = (field - ip::kEnvelopeFirst) % ip::kEnvelopeStride;
    return f >= ip::EnvAmount && f < ip::kEnvelopeFields;
  }
  return false;  // the source page, and any id the table does not have
}

unsigned param_slots_per_index(core::ParamKind kind) noexcept {
  switch (kind) {
    case core::ParamKind::Instrument: return instrument_param::kCount;
    case core::ParamKind::Track: return track_param::kCount;
    case core::ParamKind::Master: return master_param::kCount;
    case core::ParamKind::Project: return project_setting::kCount;
    case core::ParamKind::Device: return device_setting::kCount;
    case core::ParamKind::None:
    default: return 0;
  }
}

unsigned param_index_count(core::ParamKind kind) noexcept {
  switch (kind) {
    case core::ParamKind::Instrument: return kMaxInstruments;
    case core::ParamKind::Track: return kMaxTracks;
    case core::ParamKind::Master:
    case core::ParamKind::Project:
    case core::ParamKind::Device: return 1;
    case core::ParamKind::None:
    default: return 0;
  }
}

unsigned param_slot(core::ParamId id) noexcept {
  std::size_t extra;
  if (row_of(id, extra) == nullptr) {
    return kNoParamSlot;
  }
  const unsigned field = core::param_field(id);
  if (core::param_kind(id) == core::ParamKind::Instrument && instrument_param::is_midi_row(field)) {  // OS-INSTUI S8b: after the MOD rows
    return kMidiSlotBase + field - instrument_param::kMidiFirst;
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && instrument_param::is_mod_field(field)) {  // OS-INSTUI S6: after the envelope fields
    return kModSlotBase + field - instrument_param::kModFirst;  // (the stride of the ids is the stride of the slots)
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && field >= instrument_param::kEnvelopeFirst) {
    const unsigned rel = field - instrument_param::kEnvelopeFirst;
    return instrument_param::kBase + instrument_param::kSourceRows + (rel / instrument_param::kEnvelopeStride) * kEnvelopeRows + rel % instrument_param::kEnvelopeStride;
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && instrument_param::is_source_field(field)) {
    return instrument_param::kBase + (field - instrument_param::kSourceFirst);
  }
  return field;
}

unsigned param_field_of_slot(core::ParamKind kind, unsigned slot) noexcept {
  if (slot >= param_slots_per_index(kind)) {
    return 0xFFFFu;
  }
  if (kind == core::ParamKind::Instrument && slot >= instrument_param::kBase) {
    if (slot < instrument_param::kBase + instrument_param::kSourceRows) {
      return instrument_param::kSourceFirst + (slot - instrument_param::kBase);
    }
    if (slot >= kMidiSlotBase) {
      return instrument_param::kMidiFirst + (slot - kMidiSlotBase);
    }
    if (slot >= kModSlotBase) {
      return instrument_param::kModFirst + (slot - kModSlotBase);
    }
    const unsigned rel = slot - instrument_param::kBase - instrument_param::kSourceRows;
    return instrument_param::envelope(rel / kEnvelopeRows, rel % kEnvelopeRows);
  }
  return slot;
}

unsigned param_group_count(core::ParamKind kind) noexcept {
  return kind == core::ParamKind::Instrument ? instrument_param::GroupCc + 1u : (kind == core::ParamKind::Device ? 4u : (param_slots_per_index(kind) != 0 ? 1u : 0u));
}

const char* param_group_name(core::ParamKind kind, unsigned group) noexcept {
  static constexpr const char* const kInstrumentGroups[instrument_param::GroupCc + 1] = {"VOICE", "FILTER", "MOD", "SENDS", "SOURCE", "MIDI", "CC"};
  switch (kind) {
    case core::ParamKind::Instrument: return group <= instrument_param::GroupCc ? kInstrumentGroups[group] : "";
    case core::ParamKind::Track: return group == 0 ? "MIXER" : "";
    case core::ParamKind::Master: return group == 0 ? "MASTER" : "";
    case core::ParamKind::Project: return group == 0 ? "PROJECT" : "";
    case core::ParamKind::Device: return group == kGroupMidi ? "MIDI" : (group == kGroupDisplay ? "DISPLAY" : (group == kGroupUsb ? "USB" : (group == kGroupPattern ? "PATTERN" : "")));
    case core::ParamKind::None:
    default: return "";
  }
}

unsigned param_group_fields(core::ParamKind kind, unsigned group, unsigned envelope, std::uint16_t* out, unsigned cap) noexcept {
  const Row* table = nullptr;
  unsigned rows = 0;
  switch (kind) {
    case core::ParamKind::Instrument:
      if (group == instrument_param::GroupEnvelope) {
        unsigned n = 0;
        // OS-INSTUI S6: the page of a destination lists everything it can hold, in this order: SOURCE, ENABLE (the stored switch SOURCE sets; the page does not show it), the envelope's AMOUNT .. RELEASE, the
        // LFO's SHAPE, SPEED, AMOUNT (ten rows; the screen shows the ones the source says).
        if (envelope < instrument_param::kEnvelopes && n < cap) {
          out[n++] = static_cast<std::uint16_t>(instrument_param::mod(envelope, instrument_param::ModSource));
        }
        for (unsigned f = 0; f < kEnvelopeRows && n < cap && envelope < instrument_param::kEnvelopes; ++f) {
          out[n++] = static_cast<std::uint16_t>(instrument_param::envelope(envelope, f));
        }
        for (unsigned f = instrument_param::LfoShape; f < instrument_param::kModFields && n < cap && envelope < instrument_param::kEnvelopes; ++f) {
          out[n++] = static_cast<std::uint16_t>(instrument_param::mod(envelope, f));
        }
        return n;
      }
      if (group == instrument_param::GroupMidi || group == instrument_param::GroupCc) {  // OS-INSTUI S8b: the MIDI instrument's pages, in table order (the MIDI page also has the virtual CHANNEL, first: the screen puts it there)
        unsigned n = 0;
        for (unsigned f = 0; f < instrument_param::kMidiRows && n < cap; ++f) {
          if (kMidi[f].desc.group == group) {
            out[n++] = static_cast<std::uint16_t>(instrument_param::kMidiFirst + f);
          }
        }
        return n;
      }
      if (group == instrument_param::GroupSource) {  // (ids kSourceFirst..: the table's own order)
        unsigned n = 0;
        for (unsigned f = 0; f < instrument_param::kSourceRows && n < cap; ++f) {
          out[n++] = static_cast<std::uint16_t>(instrument_param::kSourceFirst + f);
        }
        if (n < cap) {
          out[n++] = static_cast<std::uint16_t>(instrument_param::PlayEngine);  // (O5: the screen puts it last, after the rows of the play mode)
        }
        return n;
      }
      table = kInstrument;
      rows = instrument_param::kBase;
      break;
    case core::ParamKind::Track: table = kTrack; rows = track_param::kCount; break;
    case core::ParamKind::Master: table = kMaster; rows = master_param::kCount; break;
    case core::ParamKind::Project: table = kProject; rows = project_setting::kCount; break;
    case core::ParamKind::Device: table = kDevice; rows = device_setting::kCount; break;
    case core::ParamKind::None:
    default: return 0;
  }
  unsigned n = 0;
  for (unsigned f = 0; f < rows && n < cap; ++f) {
    if (table[f].desc.group == group) {
      out[n++] = static_cast<std::uint16_t>(f);
    }
  }
  if (kind == core::ParamKind::Instrument && group == instrument_param::GroupVoice && n < cap) {
    out[n++] = static_cast<std::uint16_t>(instrument_param::VelSens);  // OS-INSTUI S9: VEL> closes the VOICE page of a sample instrument
  }
  return n;
}

const char* envelope_name(unsigned envelope) noexcept {
  return envelope < instrument_param::kEnvelopes ? kEnvelopeNames[envelope] : "";
}

float param_load(const void* owner, const ParamField& f) noexcept {
  const unsigned char* p = static_cast<const unsigned char*>(owner) + f.offset;
  switch (f.store) {
    case ParamStore::U8:
      return f.mask != 0 ? ((*p & f.mask) != 0 ? 1.0f : 0.0f) : static_cast<float>(*p);
    case ParamStore::I8:
      return static_cast<float>(static_cast<std::int8_t>(*p));
    case ParamStore::U16: {
      std::uint16_t v;
      std::memcpy(&v, p, sizeof v);
      return static_cast<float>(v);
    }
    case ParamStore::F32: {
      float v;
      std::memcpy(&v, p, sizeof v);
      return v;
    }
    case ParamStore::EnvState:  // p[0] is `loop`, p[1] `enable` (the assert at kModSource)
      return p[1] == 0u ? 0.0f : (p[0] != 0u ? 2.0f : 1.0f);
  }
  return 0.0f;
}

bool param_store(void* owner, const ParamField& f, float value) noexcept {
  if (!core::param_in_range(*f.desc, value)) {
    return false;  // NaN, or outside [min, max]
  }
  unsigned char* p = static_cast<unsigned char*>(owner) + f.offset;
  const long whole = static_cast<long>(value + (value < 0.0f ? -0.5f : 0.5f));  // in range: at most +-32,768 + 1
  switch (f.store) {
    case ParamStore::U8:
      if (f.mask != 0) {
        *p = static_cast<unsigned char>(whole != 0 ? (*p | f.mask) : (*p & ~f.mask));
      } else {
        *p = static_cast<unsigned char>(whole);
      }
      return true;
    case ParamStore::I8:
      *p = static_cast<unsigned char>(static_cast<std::int8_t>(whole));
      return true;
    case ParamStore::U16: {
      const std::uint16_t v = static_cast<std::uint16_t>(whole);
      std::memcpy(p, &v, sizeof v);
      return true;
    }
    case ParamStore::F32:
      std::memcpy(p, &value, sizeof value);
      return true;
    case ParamStore::EnvState:  // Off clears both, Envelope enables with no loop, LFO enables with the loop
      p[0] = static_cast<unsigned char>(whole == 2 ? 1u : 0u);
      p[1] = static_cast<unsigned char>(whole != 0 ? 1u : 0u);
      return true;
  }
  return false;
}

void reset_instrument_params(Instrument& in) noexcept {
  for (unsigned slot = 0; slot < instrument_param::kCount; ++slot) {
    const unsigned field = param_field_of_slot(core::ParamKind::Instrument, slot);
    ParamField f;
    if (field == instrument_param::Slices || !find_param(core::param_id(core::ParamKind::Instrument, 0, field), f)) {
      continue;  // (SLICES is a readout of the slices made on Wave)
    }
    (void)param_store(&in, f, f.desc->def);
  }
  for (Envelope& e : in.sampler.envelope) {  // OS-INSTUI S6: the delay and the hold are no parameters (the editor has no row for them) and INIT is every parameter of the instrument: back to 0
    e.delay = 0;
    e.hold = 0;
  }
  in.sampler.sample_type = 0;
}

}  // namespace o3os::model
