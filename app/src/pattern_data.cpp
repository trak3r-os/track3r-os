// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/pattern_data.h>

#include <o3os/app/text_format.h>
#include <o3os/ui/assets.h>

namespace o3os::app {

char fx_type_char(std::uint8_t type) noexcept {
  if (type < 10) {
    return static_cast<char>('0' + type);
  }
  if (type < 36) {
    return static_cast<char>('A' + (type - 10));
  }
  if (type < 43) {
    return "!#$%&*+"[type - 36];
  }
  if (type >= 46 && type <= 62) {
    return "@^~<>=\"';/:|[]()\x7b"[type - 46];  // OS-COREFX: tracker Arpeggio, Volume slide, Fine volume slide, Fine porta up, down; CF2: Pan slide, Vib wave, Trem wave, Glissando, Retrigger, Tremor, Channel pan; CF3: Channel volume [, slide ], Global volume (, slide ), Panbrello {
  }
  if (type >= 96 && type <= 98) {
    return "wyz"[type - 96];  // OS-CEWTENV W6b: tracker-ce step FX w (amp envelope), y (cutoff envelope), z (granular length)
  }
  if (type >= 128) {
    return static_cast<char>('a' + (type - 128) % 26);
  }
  return '?';
}

unsigned note_tile_cell(std::uint8_t note) noexcept {
  if (note <= 120) {
    return note;  // 0 is "---"
  }
  if (note <= 128) {
    return ui::kNoteTileOctave10 + (note - 121u);
  }
  switch (note) {
    case model::kNoteOff:
      return ui::kNoteTileOff;
    case model::kNoteCut:
      return ui::kNoteTileCut;
    case model::kNoteFade:
      return ui::kNoteTileFade;
    default:
      return ui::kNoteTileUnknown;
  }
}

unsigned note_tile_cell(std::uint8_t note, int octave_shift) noexcept {
  if (octave_shift == 0 || note < 1u || note > 128u) {
    return note_tile_cell(note);
  }
  const int shifted = static_cast<int>(note) + 12 * octave_shift;
  return shifted >= 1 && shifted <= 128 ? note_tile_cell(static_cast<std::uint8_t>(shifted)) : ui::kNoteTileUnknown;
}

void note_name(unsigned note, int octave_shift, char (&out)[4]) noexcept {
  ui::note_tile_text(note < 128u ? note_tile_cell(static_cast<std::uint8_t>(note + 1u), octave_shift) : ui::kNoteTileUnknown, out);
}

unsigned row_number_tile(int row, unsigned numbering) noexcept {
  const int r = row < 0 ? 0 : row;
  if (numbering == static_cast<unsigned>(Numbering::Hex)) {
    return static_cast<unsigned>(r) & 0xFFu;
  }
  const int n = numbering == static_cast<unsigned>(Numbering::From0) ? r : r + 1;
  return static_cast<unsigned>(((n / 10) << 4) | (n % 10)) & 0xFFu;
}

namespace {

bool format_note(std::uint8_t note, char* out) noexcept {
  char text[4];
  ui::note_tile_text(note_tile_cell(note), text);  // the tile's own name: what is drawn and what is said are one table
  put_str(out, text);
  return note == model::kNoteEmpty;
}

struct TrackInfo {
  const char* name;
  char type;
};
constexpr TrackInfo kTrackInfo[kTracks] = {
    {"KICK", 'A'},  {"SNARE", 'A'}, {"HAT", 'A'},    {"ACID", 'A'},   {"BASS", 'A'},   {"LEAD", 'A'},
    {"PAD", 'A'},   {"PERC", 'A'},  {"SYNTH", 'M'},  {"DRUMS", 'M'},  {"LINE L", 'E'}, {"LINE R", 'E'},
};

// Demo meter levels and peaks per track (a peak is never below its level): a plausible spread, one track silent.
constexpr std::uint8_t kLevels[kTracks] = {200, 150, 96, 224, 120, 64, 168, 0, 140, 188, 80, 104};
constexpr std::uint8_t kPeaks[kTracks] = {236, 190, 130, 255, 160, 96, 210, 0, 176, 232, 112, 140};

constexpr const char* kInstrumentNames[] = {"KICK", "SNARE", "HAT", "ACID", "BASS", "LEAD", "PAD", "PERC", "SYNTH", "DRUMS"};

// A demo track: an event every `period` steps from `phase`; the note cycles through `notes` (note 0 in the
// first slot makes a note-less track that only carries volume and FX, as on an external-audio track).
// Every fourth event carries `fx` (every event on a note-less track) and event 5 of each eight an FX2 roll.
struct Lane {
  std::uint8_t period;
  std::uint8_t phase;
  std::uint8_t instr;
  std::uint8_t vol;
  std::uint8_t fx;
  std::uint8_t fx_val;
  std::uint8_t notes[4];
};
constexpr Lane kLanes[kTracks] = {
    {4, 0, 1, 0x7F, 0, 0x00, {25, 25, 25, 25}},   // KICK: C-2
    {8, 4, 2, 0x70, 27, 0x20, {27, 27, 27, 27}},       // SNARE: D-2, low-pass
    {2, 0, 3, 0x50, 0, 0x00, {43, 43, 43, 45}},   // HAT: F#3 / G#3
    {1, 0, 4, 0x60, 128, 0x01, {13, 13, 25, 16}},      // ACID: 16ths, engine-declared accent
    {8, 0, 5, 0x78, 31, 0x40, {13, 20, 18, 15}},       // BASS
    {6, 2, 6, 0x66, 0, 0x00, {49, 52, 56, 54}},   // LEAD
    {16, 0, 7, 0x58, 22, 0x10, {37, 40, 44, 42}},      // PAD
    {3, 1, 8, 0x4C, 3, 0x04, {48, 48, 50, 48}},        // PERC
    {4, 2, 9, 0x64, 8, 0x40, {41, 44, 48, 44}},        // SYNTH (MIDI)
    {4, 2, 10, 0x7F, 0, 0x00, {13, 15, 13, 18}},  // DRUMS (MIDI)
    {8, 0, 0, 0x40, 27, 0x30, {0, 0, 0, 0}},           // LINE L (external): volume and FX only
    {16, 8, 0, 0x40, 28, 0x50, {0, 0, 0, 0}},          // LINE R (external): volume and FX only
};

}  // namespace

bool format_field(const Step& step, Column column, char (&out)[4]) noexcept {
  out[0] = out[1] = out[2] = out[3] = '\0';
  switch (column) {
    case Column::Note:
      return format_note(step.note, out);
    case Column::Instr:
      // An off, cut or fade step shows no instrument, whatever it holds.
      if (step.instr == 0 || step.note == model::kNoteOff || step.note == model::kNoteCut || step.note == model::kNoteFade) {
        put_str(out, "--");
        return true;
      }
      if (step.instr <= model::kMaxInstruments) {
        put_dec(out, step.instr, 2);
      } else {
        put_str(out, "??");
      }
      return false;
    case Column::Vol:
      if (step.vol == model::kVolumeUnset) {
        put_str(out, "--");
      } else if (step.vol <= model::kVolumeMax) {
        put_hex2(out, step.vol);
      } else {
        put_str(out, "??");
      }
      return step.vol == model::kVolumeUnset;
    case Column::Fx1:
    case Column::Fx2: {
      const model::StepFx& fx = step.fx[column == Column::Fx1 ? 0 : 1];
      if (fx.type == model::kFxNone) {
        put_str(out, "---");
      } else {
        out[0] = fx_type_char(fx.type);
        put_hex2(out + 1, fx.value);
      }
      return fx.type == model::kFxNone;
    }
  }
  return true;
}

void fill_demo(PatternData& data) noexcept {
  for (int t = 0; t < kTracks; ++t) {
    const Lane& lane = kLanes[t];
    for (int row = 0; row < kSteps; ++row) {
      Step& s = data.steps[t][row];
      s = model::kEmptyStep;
      if (row < lane.phase || (row - lane.phase) % lane.period != 0) {
        continue;
      }
      const unsigned k = static_cast<unsigned>((row - lane.phase) / lane.period);
      if (lane.notes[0] != 0) {
        s.note = lane.notes[k % 4];
        s.instr = lane.instr;
      }
      s.vol = static_cast<std::uint8_t>(lane.vol - 12u * (k % 3u));
      if (lane.fx != 0 && (k % 4 == 3 || lane.notes[0] == 0)) {
        s.fx[0] = {lane.fx, static_cast<std::uint8_t>(lane.fx_val + 4u * (k % 8u))};
      }
      if (k % 8 == 5) {
        s.fx[1] = {3, static_cast<std::uint8_t>(4u * (k / 8u + 1u))};
      }
    }
  }
}

const char* track_name(int track) noexcept {
  return (track >= 0 && track < kTracks) ? kTrackInfo[track].name : "";
}

char track_type(int track) noexcept { return (track >= 0 && track < kTracks) ? kTrackInfo[track].type : '?'; }

unsigned track_level(int track) noexcept { return (track >= 0 && track < kTracks) ? kLevels[track] : 0u; }
unsigned track_peak(int track) noexcept { return (track >= 0 && track < kTracks) ? kPeaks[track] : 0u; }

const char* instrument_name(int instrument) noexcept {
  constexpr int kNamed = static_cast<int>(sizeof(kInstrumentNames) / sizeof(kInstrumentNames[0]));
  return (instrument >= 1 && instrument <= kNamed) ? kInstrumentNames[instrument - 1] : "";
}

}  // namespace o3os::app
