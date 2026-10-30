// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/dir_scan.h>
#include <o3os/app/file_view.h>
#include <o3os/formats/format.h>

#include <cstdint>

namespace o3os::app {

// The Sample load screen's state (OS-011a): the cursor and window over the listing of the folder it browses (a FileView, as the File screen's, over the
// App's one DirScan), the folder (the card's path without a leading or trailing '/', "" for the root), and the request the actions leave for the App's pass,
// which owns the card and the jobs (the same split as FileView::rescan): enter the folder under the cursor, go up, or load the file under the cursor into an
// instrument slot.
// The card's folder of the instrument library (hal/save_guard.h kInstrumentsDir, which save_service.cpp asserts equal: this layer does not include the guard).
inline constexpr char kInstrumentsFolder[] = "instruments";

struct WavView {
  enum class Go : std::uint8_t { None, Enter, Up, Load, Preview, LoadMarked };

  FileView list;
  char folder[128] = "";  // (a folder deeper than that is not entered: "THE PATH IS TOO LONG")
  Go go = Go::None;
  // OS-AUDIT-A1 (E-LOAD-06, E-LOAD-23; new Community OS behaviour): the name of the entry the cursor goes back to when the scan of the folder ends: the folder just left (UP), or the entry the
  // cursor was on when the File screen took the shared listing. Empty: none. Set by the App's pass, used once by follow_files() (a longer name than this is not remembered).
  char reselect[64] = "";
  bool audition_held = false;  // OS-FU2: F5 is down (set by the action, cleared by the key's release): the audition lasts as long as it does
  std::uint8_t slot = 0;  // Go::Load: the instrument slot (0-based) the file goes to; Go::LoadMarked: the slot the free ones are looked for from
  // OS-AUDIT-A7 (SLD-MULTI, E-LOAD-14 / E-LOAD-15): the files marked for one multi-load, a bit per row of the listing (a folder cannot be marked). Cleared by every new scan of the listing (a folder change,
  // RESCAN, a card change) and when the screen is left; a file's mark goes when its load is tried, so what stays marked after a load is what did not get a slot. Written by the actions and the App's pass, both
  // on the main loop.
  static constexpr unsigned kMarkWords = DirIndex::kMaxEntries / 32u;
  std::uint32_t marks[kMarkWords] = {};
  bool marked(unsigned pos) const noexcept { return pos < DirIndex::kMaxEntries && ((marks[pos / 32u] >> (pos % 32u)) & 1u) != 0u; }
  void set_mark(unsigned pos, bool on) noexcept {
    if (pos < DirIndex::kMaxEntries) {
      marks[pos / 32u] = on ? (marks[pos / 32u] | (1u << (pos % 32u))) : (marks[pos / 32u] & ~(1u << (pos % 32u)));
    }
  }
  unsigned mark_count() const noexcept {
    unsigned n = 0;
    for (const std::uint32_t w : marks) {
      n += static_cast<unsigned>(__builtin_popcount(w));
    }
    return n;
  }
  int first_mark() const noexcept {  // the lowest marked row, -1 when none
    for (unsigned i = 0; i < kMarkWords; ++i) {
      if (marks[i] != 0u) {
        return static_cast<int>(i * 32u + static_cast<unsigned>(__builtin_ctz(marks[i])));
      }
    }
    return -1;
  }
  void clear_marks() noexcept {
    for (std::uint32_t& w : marks) {
      w = 0u;
    }
  }
  // OS-BUG-SLDOVER: F1 over an instrument that is there asks first ("REPLACE 03 KICK: F1 AGAIN", Ask::Replace); the second F1 loads when it is for the same slot and the same list position, within kAskMs,
  // and the question is still the head of the strip (found again by ask_text, as the OG row's). ask_pos -1: no question.
  static constexpr std::uint32_t kAskMs = 4000;
  int ask_pos = -1;
  std::uint8_t ask_slot = 0;
  std::uint32_t asked_at = 0;
  core::FixedString<core::kNotifyTextChars> ask_text;
  bool pti = false;  // OS-INSTUI S7: the screen lists instrument files (.pti) and F1 loads one into the instrument in view (opened by the type chooser's LOAD); cleared when the screen is left
  bool back_to_params = false;  // OS-INSTUI S5: the screen was opened by the type chooser of an empty slot; a load that is done goes back to the Params page (cleared when the screen is left)
  // OS-GRAN: how a WAV is read as a wavetable (F7; formats::ReadParams::wavetable, wav_sample.h WAVETABLE FRAMES): kWtAuto (only a marked WAV, Serum's), kWtDetect (the frame size by
  // the length: 2048, 1024 or 256), an explicit 256 .. 2048, kWtOff. A Surge .wt is a wavetable whatever this says. Not saved: a loading preference of the screen.
  std::uint16_t wavetable = formats::kWtAuto;
  // OS-LOADCH: which channel of a stereo file the next LOAD / ADD (and the audition) takes: 0 MIX ((L + R) / 2, as before), 1 LEFT, 2 RIGHT (formats::ReadParams::channel). F8 cycles it while the file under
  // the cursor is stereo (DirIndex::stereo); a mono file ignores it. Remembered for the session, never saved.
  // OS-STEREO (b), S8: 3 is STEREO (kChannelStereo): the load keeps the mid (the same sample as MIX) AND the side signal, and sets the instrument's stereo flag (ReadParams::side). The audition plays the mix.
  std::uint8_t channel = 0;
};

// F8's cycle (MIX, LEFT, RIGHT, STEREO, round), its key label (at most 8 characters) and its notice.
inline constexpr std::uint8_t kChannelStereo = 3;
inline std::uint8_t next_channel(std::uint8_t c) noexcept { return c >= kChannelStereo ? std::uint8_t{0} : static_cast<std::uint8_t>(c + 1u); }
inline const char* channel_key_label(std::uint8_t c) noexcept { return c == 1u ? "CH LEFT" : (c == 2u ? "CH RIGHT" : (c == kChannelStereo ? "STEREO" : "CH MIX")); }
inline const char* channel_notice(std::uint8_t c) noexcept {
  return c == 1u ? "STEREO FILE LOADS: LEFT" : (c == 2u ? "STEREO FILE LOADS: RIGHT" : (c == kChannelStereo ? "STEREO FILE LOADS: STEREO (MID + SIDE)" : "STEREO FILE LOADS: MIX (L+R)/2"));
}

// F7's cycle and its words: AUTO, LEN, 256, 512, 1024, 2048, OFF, and round (the key label, at most 8 characters; the notice).
inline std::uint16_t next_wavetable_mode(std::uint16_t m) noexcept {
  switch (m) {
    case formats::kWtAuto: return formats::kWtDetect;
    case formats::kWtDetect: return 256;
    case 256: return 512;
    case 512: return 1024;
    case 1024: return 2048;
    case 2048: return formats::kWtOff;
    default: return formats::kWtAuto;
  }
}
inline const char* wavetable_key_label(std::uint16_t m) noexcept {
  switch (m) {
    case formats::kWtDetect: return "WT LEN";
    case 256: return "WT 256";
    case 512: return "WT 512";
    case 1024: return "WT 1024";
    case 2048: return "WT 2048";
    case formats::kWtOff: return "WT OFF";
    default: return "WT AUTO";
  }
}
inline const char* wavetable_notice(std::uint16_t m) noexcept {
  switch (m) {
    case formats::kWtDetect: return "WAV AS WAVETABLE: FRAME BY LENGTH";
    case 256: return "WAV AS WAVETABLE: 256 FRAMES";
    case 512: return "WAV AS WAVETABLE: 512 FRAMES";
    case 1024: return "WAV AS WAVETABLE: 1024 FRAMES";
    case 2048: return "WAV AS WAVETABLE: 2048 FRAMES";
    case formats::kWtOff: return "WAV NEVER A WAVETABLE";
    default: return "WAV AS WAVETABLE: ONLY IF MARKED";
  }
}

}  // namespace o3os::app
