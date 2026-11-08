// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The value of a pattern step FX in words (OS-PFX; docs/ui/ui-map.md section 4: "a value formatter each for the popup (chord names, arp patterns)"): what the Pattern screen's info line
// says about the FX under the cursor, so that a hex byte (the grid's own text) is also a chord, an arpeggio, a tuning or a distance. Only the FX whose value is not a plain number have one:
//   Chord 14   "CHORD MAJ7 47A" / "CHORD 28" / "CHORD OFF"   the 30 chords (seq/fx.h kChordMask): the intervals above the root as a label (one hex digit each: 47A is 4, 7, 10), and
//              the name where this OS gives one (11 of the 29: docs/specs/OS-PFX.md)
//   Arp 21     "ARP UP 8 STEPS" / "ARP DOWN 1/2 STEP" / "ARP RND 1 STEP" / "ARP OFF"    direction and the time between two notes (seq/fx.h kArpPeriod: 384 .. 6 microsteps of 48 a step)
//   Tune 40    "TUNE +7 ST" / "TUNE 0 ST" / "TUNE -12 ST"
//   Slide 41/42 "SLIDE UP 2.19 ST" / "SLIDE DOWN 0.50 ST"  the distance in semitones to two places (the value is sixteenths of a semitone)
//   Glide 19   "GLIDE OFF" / "GLIDE 240 MS" / "GLIDE 1.28 S"   the time the value means (audio/events.h glide_fx_ms)
//   Chance 4   "CHANCE 50 %" / "CHANCE WITH LAST" / "CHANCE NOT LAST" / "CHANCE PLAY 2 SKIP 3" / "CHANCE SKIP 2 PLAY 3"   (OS-CECHANCE C2: 0..100 a probability,
//              101..174 the conditions; the words are this OS's, tracker-ce's grid labels GL / !L / n:m / Snm are not copied)
//   Overdrive 38 "DRIVE 40 %" / "DRIVE OFF"   Bit depth 39 "BITS 8" / "BITS 16 (OFF)"   (OS-IFX1)
//   Tracker Arp 46 "ARPEG +3 +7" / "ARPEG OFF"   Volume slide 47 "VSLIDE +4" / "VSLIDE -4" / "VSLIDE 0"   Fine volume slide 48 "FVOL +2" / "FVOL -2" / "FVOL 0"
//   Fine porta 49 / 50 "FINE UP 1.00 PER" / "FINE DOWN 0.25 PER"   the Paula period units (the value is quarters of one)   (OS-COREFX; x is the high nibble, y the low one)
//   CF2: Pan slide 51 "PANSL R 4" / "PANSL L 4" / "PANSL FINE R 3" / "PANSL 0"   Vibrato / Tremolo waveform 52 / 53 "VIB SINE" / "VIB RAMP" / "TREM SQUARE" / "TREM RANDOM NORETRIG"
//   Glissando 54 "GLISS ON" / "GLISS OFF"   Retrigger 55 "RETRIG EVERY 3" / "RETRIG EVERY 2 VOL -4" / "RETRIG EVERY 1 VOL X1/2"   Tremor 56 "TREMOR ON 2 OFF 1"   Channel pan 57 "CHPAN 75"
//   CF3: Channel volume 58 "CHVOL 48"   Channel volume slide 59 "CHVSL -3" / "CHVSL FINE +2" / "CHVSL 0"   Global volume 60 "GVOL 64"   Global volume slide 61 "GVSL +4" / "GVSL FINE -1"   Panbrello 62 "PANBR SPD 3 DEP 5" / "PANBR SPD 3 DEP KEEP"
// The words are this OS's (the OG's own text is not reused). Pure, no heap; the value is read as the sequencer reads it (clamped to the type's range).

#include <o3os/app/text_format.h>
#include <o3os/audio/events.h>
#include <o3os/core/fixed_string.h>
#include <o3os/seq/fx.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// OS-FLASHDIET P3: the FX picker's names and descriptions are one deflate stream (tools/fx_text_pack.py packs app/fx/fx_text.txt into the generated fx_text_blob.cpp), not a C++ table: the
// text of the names, one per line, then the descriptions, one per line. A reader decodes up to its line with the UI's shared decoder (ui::asset_inflater()): main loop only, synchronously.
extern const std::uint8_t* const kFxTextZ;  // the raw-deflate stream (word aligned)
extern const std::size_t kFxTextZSize;      // its size in bytes
extern const std::size_t kFxTextRawSize;    // the inflated text

// The caller's buffer sizes, with the terminator (tools/fx_text_pack.py refuses a longer line).
inline constexpr unsigned kFxNameMax = 24;
inline constexpr unsigned kFxDescMax = 144;

// The decoded text, kept so that a redraw does not restart the stream (main loop only, synchronous, like help_page_buf()): every name, each ended by NUL, decoded once on the first lookup, and the
// last description looked up (key = its index). On the MK66 image the platform owns it in the SDRAM slack window (platform/mk66/src/app_main.cpp defines fx_text_cache(); app/CMakeLists.txt sets
// O3OS_FX_TEXT_CACHE_EXTERNAL); host, the simulator and QEMU keep app's own. All-zero is the empty state.
inline constexpr unsigned kFxNamesBytes = 768;  // the names are about 690 bytes with their terminators; a longer set is not cached (each lookup streams, as before)
struct FxTextCache {
  char names[kFxNamesBytes];
  char desc[kFxDescMax];
  std::uint16_t desc_index;  // the entry desc[] holds, when desc_ok
  std::uint8_t names_state;  // 0 not decoded yet, 1 names[] holds every name, 2 it does not fit or the stream failed: each lookup streams
  std::uint8_t desc_ok;
};
FxTextCache& fx_text_cache() noexcept;

// The name of the FX picker's entry `index` (model/fx_table.h kPickerOrder order, 0 is "None"; an index past the list is None) into `out` (`cap` bytes, kFxNameMax holds every one; cut and terminated at cap):
// "Micro-tune", "Random Instrument". Returns the length (0 and an empty string if the stream is unreadable). The step popup's list and the Fill dialog's FX TYPE row (OS-FILL2) read the one text.
unsigned fx_picker_name(unsigned index, char* out, unsigned cap) noexcept;
// The description of the entry `index`, as above (kFxDescMax holds every one): the line under the popup's list.
unsigned fx_picker_desc(unsigned index, char* out, unsigned cap) noexcept;

// The name of a chord value, where this OS gives one (docs/specs/OS-PFX.md); nullptr for the other 18.
constexpr const char* chord_name(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return "SUS2";
    case 4: return "MIN";
    case 5: return "MAJ";
    case 7: return "SUS4";
    case 9: return "OPEN4";
    case 11: return "OPEN5";
    case 19: return "MIN6";
    case 20: return "MIN7";
    case 22: return "MAJ6";
    case 23: return "MAJ7";
    case 24: return "MAJMAJ7";
    default: return nullptr;
  }
}
// By arp rate (seq/fx.h kArpPeriod): the steps between two notes.
inline constexpr const char* kArpRateNames[seq::kArpRates] = {"8", "6", "4", "3", "2", "1", "1/2", "1/3", "1/4", "1/6", "1/8"};

namespace detail {
// The one body (app/src/fx_text.cpp, OS-FLASHDIET P2): writes into `buf` (cap characters and a terminator) at `len`, cut at cap like FixedString.
bool fx_value_text(char* buf, std::size_t cap, std::size_t& len, std::uint8_t type, std::uint8_t value) noexcept;
}  // namespace detail

// Appends the words for (type, value); false and nothing appended for a type without a formatter. A forwarder over the one body, whatever the capacity.
template <std::size_t N>
[[gnu::always_inline]] inline bool append_fx_value(core::FixedString<N>& s, std::uint8_t type, std::uint8_t value) noexcept {
  return detail::fx_value_text(core::detail::FixedStringAccess::buf(s), N, core::detail::FixedStringAccess::len(s), type, value);
}

}  // namespace o3os::app
