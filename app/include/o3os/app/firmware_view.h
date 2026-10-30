// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The firmware update dialog (OS-FWCONFIG slice 2a, docs/specs/OS-FWCONFIG.md; NEW COMMUNITY OS BEHAVIOUR, Level C): F8 UPDATE on the INFO page of Config opens one modal dialog that walks the whole
// path. It sees the update path through hal::firmware::FirmwareSink and nothing else (the check, the second check and the hand-over are the console's own code behind the port). The phases:
//
//   List        the *.ptf files of Firmware/, a window of kMax of them copied in ONE pass of the main loop when the dialog opens (the card has one directory handle). Up / Down and the encoder choose, F1 UPDATE
//               asks, F8 BACK closes. A cursor that leaves the window loads the window it moves into (OS-BUG-FWLIST): one more pass over the folder that skips the entries before it, so every file can be reached.
//   AskUpdate   question 1, "UPDATE TO <name>?": F1 YES starts the check. Any other key takes the question back (the list again); nothing was started.
//   Checking    the file is read and validated on the card, a slice a pass (App::poll_firmware): a bar, F8 CANCEL.
//   AskInstall  question 2, "IMAGE OK, <length> B, CRC <8 hex>": F1 INSTALL within kCheckLifetimeMs. Any other key, the timeout or the card going to a computer takes it back. Nothing is written yet.
//   Installing  INSTALL was taken: the re-check, the transport stopped, _fwinfo written, the pin. "DO NOT POWER OFF". No key acts; the chip resets on success. On a failure the list comes back with the reason.
//
// Every way out of the dialog calls FirmwareSink::cancel() (a running check holds the card; a good one that is left waiting keeps the HIL key injection refused until it lapses): BACK, the
// question taken back, a card removal, an error, and the App's pass when it finds the overlay gone for any other reason.

#include <o3os/core/fixed_string.h>
#include <o3os/core/tick.h>
#include <o3os/hal/firmware.h>

#include <cstdint>

namespace o3os::app {

struct FirmwareView {
  enum class Phase : std::uint8_t { Closed, List, AskUpdate, Checking, AskInstall, Installing };
  static constexpr unsigned kMax = 12;   // files kept at a time: the window (the others are loaded when the cursor gets to them)
  static constexpr unsigned kScanMax = 999;  // entries of the folder that a pass counts and pages through (a bound for the pass, far above any card's folder)
  static constexpr std::uint16_t kNoWant = 0xFFFFu;
  static constexpr unsigned kRows = 8;   // rows on screen at a time
  static constexpr unsigned kMsgChars = 86;  // one line of the dialog: 87 cells of the Label face (the dialog is 700 px wide inside its margins), so a text cut here still fits and is never wrapped
  using Text = core::FixedString<kMsgChars>;  // the one string type of the dialog (its line, a size, a question): one instantiation

  Phase phase = Phase::Closed;
  std::uint8_t n = 0;          // files in the window
  std::uint8_t cursor = 0;     // in the window; the entry of the folder is first + cursor
  std::uint16_t first = 0;     // the folder's index of files[0] (a multiple of kMax)
  std::uint16_t entries = 0;   // the files of the folder (at most kScanMax) as the last pass saw them
  std::uint16_t want = kNoWant;  // the folder's index the cursor was sent to, outside the window: the App's next pass (or F1) loads that window (a cursor action has no sink); main loop only
  bool bad = false;            // `msg` is a refusal or a failure (drawn in the accent colour)
  hal::firmware::File files[kMax] = {};
  std::uint32_t done = 0;      // Checking and Installing: bytes read
  std::uint32_t total = 0;
  std::uint32_t image = 0;     // AskInstall: the image's length
  std::uint32_t crc = 0;       // ... and the file's CRC-32
  core::Tick asked_at = 0;     // AskInstall: when the check ended (the question lasts kCheckLifetimeMs)
  Text msg;                    // the line of the dialog: why a step was refused or failed

  bool open() const noexcept { return phase != Phase::Closed; }
  // The file the questions and the check are about: the one under the cursor. The cursor cannot move while a question or a check is up (every other control takes the question back,
  // and only the list moves it), so the list entry is the name for the whole walk; there is no second copy of it.
  const char* chosen() const noexcept { return files[cursor < n ? cursor : 0].name; }
  // Where the cursor is, or is going: the folder's index.
  unsigned position() const noexcept { return want != kNoWant ? want : static_cast<unsigned>(first) + cursor; }
  // Both questions are up in these two phases; the key that answers them is F1.
  bool asking() const noexcept { return phase == Phase::AskUpdate || phase == Phase::AskInstall; }
  void close() noexcept {
    phase = Phase::Closed;
    n = 0;
    cursor = 0;
    first = 0;
    entries = 0;
    want = kNoWant;
    bad = false;
    done = total = image = crc = 0;
    msg.clear();
  }
};

}  // namespace o3os::app
