// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-CEWTENV W8 (docs/specs/OS-CEWTENV.md F3, 3.9): the user contours. `/Envelopes/NNN.wav` (or `N.wav`) replaces the user contour U(NNN) of the wavetable bank
// (audio/wt_contour.h, rows 128..255). The rules (F3): numbered 1-based (`001.wav` is U001, `128.wav` U128), 0-based when `000.wav` or `0.wav` exists (`000.wav` is U001,
// `127.wav` U128); PCM 8, 16 or 24 bit, a stereo file takes its LEFT channel; any length is resampled to 128 points (linear); a file with a sample below -8 (-72 dBFS: clear of the 24-bit dither) is bipolar and is
// mapped from -1..1 to 0..1, a file without one is taken as it is (0..1). A number with no file keeps the procedural fallback. New Community OS behaviours on top of the cited rules:
//   * a float WAV is REFUSED and counted (the contour stays the fallback), the report says so;
//   * a file of more than kWtUserPcmFrames frames (0.56 s at 44.1 kHz) is refused as too long, not clipped (the scratch is 48 KiB of SDRAM, never SRAM);
//   * a file that fails (not a WAV, truncated, too long, a number out of range, a second file for the same number) never stops the others: it is counted and the slot keeps the
//     fallback; of two files for one number the first that loads, in the directory's order, wins (the second is counted, not read);
//   * a load is a pass over the whole folder: a slot that held a file and has none now (the card was swapped, another project) goes back to its fallback.
//
// MAIN LOOP ONLY, read only (hal::storage). The load is synchronous: one scan of the directory to find the numbering, one to load, at most 128 files of at most
// kWtUserMaxFileBytes (about 150 KiB) read through the WAV importer: it runs after a card mount and after a project load, never in the render. Each contour is written to the spare of
// its slot's pair and published with one aligned 32-bit pointer store (audio::wt_user_set), so the render reads a whole contour or the old one.

#include <o3os/audio/wt_contour.h>
#include <o3os/hal/storage.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kWtUserPcmFrames = 24576u;  // the scratch the importer converts a file into: the longest file taken (mono frames)
// The largest file the loader opens: 24-bit stereo at kWtUserPcmFrames frames and 2 KiB of header and other chunks. A larger one is refused as too long without reading it.
inline constexpr std::uint32_t kWtUserMaxFileBytes = kWtUserPcmFrames * 2u * 3u + 2048u;
inline constexpr const char* kWtUserFolder = "Envelopes";  // relative to the card root, as the app's "Samples" and "Projects" are: the card's /Envelopes

// The loader's storage, one object in SDRAM (the reverb region's spare tail on the unit, mem_map.h kWtUserBase; a member of the simulator): the bank's user table and the scratch.
struct WtUserMem {
  audio::WtUserBank bank;
  alignas(4) std::int16_t pcm[kWtUserPcmFrames];
};
static_assert(alignof(WtUserMem) >= 4, "word aligned");
static_assert(sizeof(void*) != 4u || sizeof(WtUserMem) == 82448u, "mem_map.h kWtUserBytes mirrors this size (on the 32-bit target; a host pointer is 8 bytes)");

// What a load did. `folder`: /Envelopes exists. `zero_based`: 000.wav or 0.wav is there. Every counter is of FILES with a numeric name; other names are not counted.
struct WtUserReport {
  bool folder = false;
  bool zero_based = false;
  std::uint16_t loaded = 0;        // contours now taken from a file
  std::uint16_t refused_float = 0; // float WAV
  std::uint16_t too_long = 0;      // over kWtUserPcmFrames frames (or kWtUserMaxFileBytes)
  std::uint16_t out_of_range = 0;  // a number past U128 (129..999 in a 1-based folder, 128..999 in a 0-based one)
  std::uint16_t duplicate = 0;     // a second file for a number already taken in this pass
  std::uint16_t bad = 0;           // not a readable 8/16/24-bit PCM WAV (the importer's refusal, a storage error, a silent file of no frames)
  std::uint16_t restored = 0;      // slots that held a file and went back to the fallback
  std::uint16_t skipped() const noexcept { return static_cast<std::uint16_t>(too_long + out_of_range + duplicate + bad); }
  bool anything() const noexcept { return loaded != 0 || refused_float != 0 || skipped() != 0 || restored != 0; }
};

// The file number of a name: 1..3 digits then `.wav` (any case). False for any other name. Pure.
bool wt_user_parse_name(const char* name, unsigned& number) noexcept;

// The 128 unipolar points (0..32767) of `frames` mono int16 frames (frames >= 1): linear resampling, then the bipolar mapping when any frame is below -8. Pure, no allocation.
void wt_user_make_points(const std::int16_t* pcm, unsigned frames, std::int16_t* out) noexcept;

// Scans kWtUserFolder and loads what is there into mem.bank (which must be bound: audio::wt_user_bind). Slots without a file go back to their fallback. `storage` not mounted: nothing
// changes, report.folder false.
WtUserReport wt_user_load(hal::storage::Storage& storage, WtUserMem& mem) noexcept;

// The strip's text of a report ("ENVELOPES: 12 LOADED, 1 FLOAT, 3 SKIPPED"), at most `cap` - 1 characters and a NUL. Returns the length; 0 (and "") when the report has nothing
// to say (no folder, or a folder with no numbered file and nothing restored).
std::size_t wt_user_notice(const WtUserReport& report, char* out, std::size_t cap) noexcept;

}  // namespace o3os::app
