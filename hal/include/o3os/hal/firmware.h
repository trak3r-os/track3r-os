// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The firmware update port (OS-FWCONFIG): what a screen may ask of the update path, and nothing else. The App sees this contract only. It cannot name the
// card's writer, the bootloader request pin or the hand-over; on the MK66 an adapter (platform/mk66/update/fw_sink.h) drives FwService's shell-free client API, so the
// screen and the `fw` console share ONE check and ONE hand-over, and the host and the simulator get a fake (platform/host/fake_firmware.h) that never touches a flash.
//
// Main loop only. Nothing waits for the card for long, but nothing is instant either: a list_next() or a poll_check() slice reads the card (2 KiB of the file), and the poll that
// starts the hand-over writes _fwinfo, syncs and reads it back, then lets the console's output drain for up to 40 ms before the pin goes low. The screen budgets one such pass; it
// never calls these from the audio path or an interrupt.
//
// The sequence:
//   list_begin() / list_next() / list_end()   the *.ptf files of Firmware/ in directory order (name, size, the first one flagged: the reset button's target). The card has ONE
//                                             directory handle, shared with the console's `fw`: copy the whole list in ONE pass of the main loop (begin, next until false,
//                                             end) and never hold it across passes
//   begin_check(name)                         the file is read and validated on the card (the rules of tools/check_image.py), sliced
//   poll_check()                              once per pass: Running with progress, then Checked (the image's length and the file's CRC-32) or Failed (why, at which byte).
//                                             A Checked result is good for kCheckLifetimeMs; after it, or once the card went to a computer, poll_check() says Failed
//   install()                                 only after Checked: validates the file once more, stops the transport, writes _fwinfo and asks the bootloader. It starts the
//                                             job and returns None; poll_check() then reports Running with `installing` (the re-check, then the wait) and never returns
//                                             Checked again. On success the chip resets and nothing more is seen; on failure poll_check() says Failed, or DidNotStart when
//                                             the bootloader did not take over within 5 s. A refusal is the return value and nothing was touched. Busy (a check or an install is running)
//                                             changes nothing, status included: a second install() cannot hide the job's "do not power off"
//   cancel()                                  drops a check, or a result waiting for install(); an install that has asked the bootloader cannot be cancelled. REQUIRED on every
//                                             way out of the screen (a running check holds the card; a waiting result keeps the HIL key injection refused until it lapses)
// Nothing is written to the card before install().

#include <cstddef>
#include <cstdint>

namespace o3os::hal::firmware {

inline constexpr std::size_t kNameCap = 53;                   // the longest image name (48-character stem + ".ptf") and a NUL
// The longest file name, ".ptf" included, that an update may be armed for (OS-BUG-FWNAMELEN; NEW COMMUNITY OS BEHAVIOUR, the limit is OBSERVED, the bootloader's rule is an open research question).
// On the unit (2026-10-31) the SD bootloader flashed the FIRST .ptf of Firmware/, with no error, when the named file's name had 33 characters; names of 21, 29 and 30 installed (the limit lies
// between 30 and 33). So a longer file may be stored and listed but is never armed: the guard (platform/mk66/update/path_guard.h update_name_ok) and the dialog both use this number.
inline constexpr std::size_t kUpdateNameMax = 30;
inline constexpr std::uint32_t kCheckLifetimeMs = 30000;       // a Checked result is good this long (the console's token lifetime)

struct File {
  char name[kNameCap];
  std::uint32_t size;
  bool reset_target;  // the first .ptf of the folder: the one the reset button flashes
};

// Why a call was refused (the strip names it). None: it was taken.
enum class Refusal : std::uint8_t {
  None,
  NoPath,       // this build has no update path
  OnComputer,   // the card is handed to a computer
  StorageBusy,  // a project is loading or a save is running
  Busy,         // another update operation is running (the console's `fw`, a check, or an install): the call changed nothing, and the running job's status is as it was
  NoCard,       // no card, or not mounted
  NotWritable,  // write protected, exFAT, or the firmware writer is off
  NoFolder,     // no Firmware/ folder
  NotFound,
  BadName,      // a name the writer does not accept (the bootloader could not be given it)
  NameTooLong,  // a valid name over kUpdateNameMax characters: the bootloader may flash another file instead (OS-BUG-FWNAMELEN)
  NotChecked,   // install() without a good check: none made, cancelled, the card went to a computer or was changed since
  Expired,      // install() later than kCheckLifetimeMs after the check
  Io            // the card failed
};

struct CheckStatus {
  enum class State : std::uint8_t {
    Idle,          // nothing started, or cancelled
    Running,       // a check, or (installing) the re-check and the wait for the bootloader; `done` of `total` bytes
    Checked,       // the file is a valid image: `image` bytes, CRC-32 `crc`; install() may follow
    Failed,        // `fault` says why, `offset` where in the file (a check), or an install that stopped before the bootloader was asked
    DidNotStart    // install(): the pin was driven and the bootloader did not take over in 5 s; the pin is released and _fwinfo deleted
  };
  State state = State::Idle;
  bool installing = false;      // the job is install()'s
  std::uint32_t done = 0;       // Running: bytes of the file read
  std::uint32_t total = 0;      // Running: the file's size
  std::uint32_t image = 0;      // Checked: the image's length
  std::uint32_t crc = 0;        // Checked: the CRC-32 of the whole file, as `fw arm` prints it
  std::uint32_t offset = 0;     // Failed: the byte of the file where the fault was found
  bool offset_known = true;     // Failed: `offset` is a place in the file; false for a fault of the whole image (not padded), where the stream's byte count means nothing to the user
  const char* fault = nullptr;  // Failed: a short static text ("flash configuration field is not the required one"); nullptr otherwise
};

class FirmwareSink {
 public:
  // False where there is no update path (the screen shows no row).
  virtual bool available() const noexcept = 0;

  // The *.ptf files of Firmware/ in directory order; the temporary and hand-over files are not listed. None from list_begin() means list_next() may be called until it says
  // false; list_end() always follows a successful list_begin().
  virtual Refusal list_begin() noexcept = 0;
  virtual bool list_next(File& out) noexcept = 0;
  virtual void list_end() noexcept = 0;

  virtual Refusal begin_check(const char* name) noexcept = 0;
  virtual CheckStatus poll_check() noexcept = 0;
  virtual void cancel() noexcept = 0;
  virtual Refusal install() noexcept = 0;

 protected:
  FirmwareSink() = default;
  ~FirmwareSink() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  FirmwareSink(const FirmwareSink&) = delete;
  FirmwareSink& operator=(const FirmwareSink&) = delete;
};

}  // namespace o3os::hal::firmware
