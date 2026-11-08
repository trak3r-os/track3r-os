// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Interoperability data, tables of facts and not code: the names, ranges and masks below are this OS's own tables. The table is "No scale" (index 0, the Fill dialog's "no filter") followed by
// the official firmware's 39 scales in the official order (verified parity, compared row by row: tracker-ce@7fd6c2c:core/source/scales.h:11-60), so **core index = official index + 1** and an
// official project's scale byte (0..38) is this table's index - 1; that byte is the NOTE 1.1 scale of OS-SCALES28. 38 of the 39 masks are equal to the official ones; **Todi (23) differs on
// purpose**: the official row is a copy of its Phrygian (0 1 3 5 7 8 10), ours keeps the standard definition 0 1 3 6 7 8 11 (OS-PTOOLS, new Community OS behaviour). Melo Min (11), Blues Maj (17)
// and Gipsy (36) equal the official rows. `scale_corrected` says which rows are set to the standard definitions of their names.
//
// The pad board's scales (OS-009 P1a.1, PAR-073): 40 scales, each a degree list (below). A mask has bit k set when the note k
// semitones above the root belongs to the scale (bit 0 is the root and is always in).
//
// Pure data, no heap. The scales are the standard pitch-class sets of their names (a fact of music theory, written here as degree lists); their order is the scale index
// that project files store (the pad board's scale), so it cannot change; the names are the one table of labels the whole OS shows (OS-SCALES28 Q4c): the Fill dialog's KEY row and
// the held Note panel's top line and info line, at most 11 characters each, no trailing space.

#include <cstdint>

namespace o3os::core {

inline constexpr unsigned kScaleCount = 40;      // scale indices 0..39
inline constexpr unsigned kScaleNameChars = 11;  // the longest name; a name is stored with its terminator (12 bytes)

// The display names (OS-SCALES28 section 3.1): "No scale" plus the 39 scales of the official order, in the panel's readable style. One scale has one name across the OS.
inline constexpr char kScaleNames[kScaleCount][kScaleNameChars + 1] = {
    "No scale",   "Chromatic",  "Minor",       "Major",       "Dorian",     "Lydian",     "Lyd. minor",  "Locrian",    "Phrygian",   "Phryg. dom.",
    "Mixolydian", "Mel. minor", "Harm. minor", "Bebop major", "Bebop dor.", "Bebop mix.", "Blues",       "Blues major", "Pent. min", "Pent. maj",
    "Hung. minor", "Ukrainian", "Marva",      "Todi",        "Whole tone", "Diminished", "Super loc.",  "Hirajoshi",  "In sen",     "Yo",
    "Iwato",      "Whole-half", "Kumoi",      "Overtone",    "Dbl. harm.", "Indian",     "Gypsy minor", "Neap. major", "Neap. minor", "Enigmatic",
};

// A scale as the semitone degrees above its root that it contains (0, the root, is always in): bit d of the mask is degree d.
template <typename... Degrees>
constexpr std::uint16_t degrees_mask(Degrees... degrees) noexcept {
  return static_cast<std::uint16_t>((... | (1u << degrees)));
}

// The degrees of each scale are the standard definitions of its name; the order is the index a project stores (the pad board's scale).
inline constexpr std::uint16_t kScaleDef[kScaleCount] = {
    degrees_mask(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11),         // 0 No scale
    degrees_mask(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11),         // 1 Chromatic
    degrees_mask(0, 2, 3, 5, 7, 8, 10),                         // 2 Minor
    degrees_mask(0, 2, 4, 5, 7, 9, 11),                         // 3 Major
    degrees_mask(0, 2, 3, 5, 7, 9, 10),                         // 4 Dorian
    degrees_mask(0, 2, 4, 6, 7, 9, 11),                         // 5 Lyd Maj
    degrees_mask(0, 2, 4, 6, 7, 8, 10),                         // 6 Lyd Min
    degrees_mask(0, 1, 3, 5, 6, 8, 10),                         // 7 Locrian
    degrees_mask(0, 1, 3, 5, 7, 8, 10),                         // 8 Phrygian
    degrees_mask(0, 1, 4, 5, 7, 8, 10),                         // 9 PhrygDom
    degrees_mask(0, 2, 4, 5, 7, 9, 10),                         // 10 Mixlydian
    degrees_mask(0, 2, 3, 5, 7, 9, 11),                         // 11 Melo Min (corrected)
    degrees_mask(0, 2, 3, 5, 7, 8, 11),                         // 12 Harm Min
    degrees_mask(0, 2, 4, 5, 7, 8, 9, 11),                      // 13 BeBopMaj
    degrees_mask(0, 2, 3, 4, 5, 7, 9, 10),                      // 14 BeBopDor
    degrees_mask(0, 2, 4, 5, 7, 9, 10, 11),                     // 15 BeBopMix
    degrees_mask(0, 3, 5, 6, 7, 10),                            // 16 Blues Min
    degrees_mask(0, 2, 3, 4, 7, 9),                             // 17 Blues Maj (corrected)
    degrees_mask(0, 3, 5, 7, 10),                               // 18 Penta Min
    degrees_mask(0, 2, 4, 7, 9),                                // 19 Penta Maj
    degrees_mask(0, 2, 3, 6, 7, 8, 11),                         // 20 Hung Min
    degrees_mask(0, 2, 3, 6, 7, 9, 10),                         // 21 Ukrainian
    degrees_mask(0, 1, 4, 6, 7, 9, 11),                         // 22 Marva
    degrees_mask(0, 1, 3, 6, 7, 8, 11),                         // 23 Todi (corrected)
    degrees_mask(0, 2, 4, 6, 8, 10),                            // 24 Wholetone
    degrees_mask(0, 2, 3, 5, 6, 8, 9, 11),                      // 25 Dim
    degrees_mask(0, 1, 3, 4, 6, 8, 10),                         // 26 SLocrian
    degrees_mask(0, 2, 3, 7, 8),                                // 27 Hirajoshi
    degrees_mask(0, 1, 5, 7, 10),                               // 28 In Sen
    degrees_mask(0, 2, 5, 7, 9),                                // 29 Yo
    degrees_mask(0, 1, 5, 6, 10),                               // 30 Iwato
    degrees_mask(0, 2, 3, 5, 6, 8, 9, 11),                      // 31 WholeHalf
    degrees_mask(0, 2, 3, 7, 9),                                // 32 Kumoi
    degrees_mask(0, 2, 4, 6, 7, 9, 10),                         // 33 Overtone
    degrees_mask(0, 1, 4, 5, 7, 8, 11),                         // 34 DoubHarm
    degrees_mask(0, 4, 5, 7, 8, 10),                            // 35 Indian
    degrees_mask(0, 2, 3, 6, 7, 8, 11),                         // 36 Gipsy (corrected)
    degrees_mask(0, 1, 3, 5, 7, 9, 11),                         // 37 NeapoMaj
    degrees_mask(0, 1, 3, 5, 7, 8, 11),                         // 38 NeapoMin
    degrees_mask(0, 1, 4, 6, 8, 10, 11),                        // 39 Enigmatic
};

// The rows PAR-073 sets to the standard definitions of their names (Todi kept so on purpose, OS-SCALES28 A8; the other three equal the official firmware's rows, tracker-ce@7fd6c2c:core/source/scales.h:11-60).
constexpr bool scale_corrected(unsigned scale) noexcept {
  return scale == 11u || scale == 17u || scale == 23u || scale == 36u;
}

// Is `note` in `scale` with `root`? Only the pitch class matters; a scale above 39 is 39.
constexpr bool is_in_scale(std::uint8_t note, std::uint8_t root, std::uint8_t scale) noexcept {
  const unsigned s = scale > kScaleCount - 1u ? kScaleCount - 1u : scale;
  const unsigned note_name = note % 12u;
  const unsigned root_name = root % 12u;
  if (note_name == root_name) {
    return true;
  }
  if (note_name > root_name) {
    return (kScaleDef[s] & (1u << (note_name - root_name))) != 0u;
  }
  return (kScaleDef[s] & (1u << ((12u - root_name) + note_name))) != 0u;
}

}  // namespace o3os::core
