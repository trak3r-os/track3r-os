// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Why the firmware writer said no (OS-006c): one small enumeration for the HEX validator, the transfer and the store, so that `fw put` prints
// `ERR <reason> at <offset>` from one table and a test can name every refusal. Ok is 0. Pure.

#include <cstdint>

namespace o3os::platform::mk66::update {

enum class Fault : std::uint8_t {
  Ok = 0,
  // the Intel HEX stream (hex_validator.h)
  HexChar,       // a byte that is not ':', a hex digit, CR or LF, or one in the wrong place
  HexRecord,     // a record shorter or longer than its byte count, or a count its type does not allow
  HexDigits,     // an odd number of hex digits on a line
  HexChecksum,   // the record's checksum byte is wrong
  HexType,       // a record type other than 00, 01, 02 and 04
  HexStart,      // a start-address record (03 or 05): the bootloader's handling of one is unknown
  HexAfterEof,   // anything but line ends after the EOF record
  HexNoEof,      // the stream ended without an EOF record
  HexOrder,      // a data byte not at the next address: a hole, a byte given twice, or records out of order
  HexRange,      // an address above 0x000FFFFF (the MK66FX1M0's 1 MiB of flash)
  HexShort,      // the image ends before 0x410: no vector table and flash configuration
  BadSp,         // the initial stack pointer is not 0x20030000
  BadReset,      // the reset vector is not a Thumb address inside the image past 0x410
  BadVector,     // a vector table entry is not a Thumb address into the image or the SRAM_L code window, or a reserved slot is not 0
  BadConfig,     // the flash configuration field is not ffffffffffffffffffffffffdef9ffff: the MCU could be locked
  // the transfer (fw_service.h)
  Idle,          // nothing arrived for 5 s
  Crc,           // the CRC-32 of what arrived is not the one announced
  Readback,      // what the card gave back is not what was sent (length, CRC or a validation of the bytes read back)
  // the store (firmware_store.h)
  Disabled,      // the writer is switched off or cannot work on this volume
  NoCard,        // no card, or it is not mounted
  WriteProtected,
  NoDir,         // Firmware/ does not exist (the writer never makes a folder)
  BadName,       // not a name the path guard accepts
  Exists,        // the target exists
  NotFound,
  Busy,          // an upload is already open
  CardBusy,      // the card is in use by the app: a project is loading or a save is running (OS-007o)
  NoSpace,       // the volume is full
  Io,            // the card or the file system failed
  Unsupported,   // exFAT, or a size outside 1 byte .. 4 MiB
  LastPtf,       // removing the last .ptf of Firmware/ is refused
  OnHost,        // the card is handed to a computer (OS-USBDISK): no `fw` command touches it until the eject
  // the screen's client API (fw_service.h, OS-FWCONFIG)
  NotArmed,      // an install without a check that is still good: none made, it was cancelled, the card went to a computer or was changed since
  Expired,       // the check is older than kArmLifetimeMs
  // the update name limit (path_guard.h, OS-BUG-FWNAMELEN)
  NameLong,      // a valid image name over kUpdateNameMax characters: never armed (the SD bootloader flashes another file for it)
  // the image's end (hex_validator.h, OS-GATE-BOOTCRASH), appended after NameLong so that no value moves
  NotPadded,     // the image's end (last address + 1) is not a multiple of 4096: the SD bootloader leaves a short final sector as it was, so the tail would be stale bytes
  Count
};

inline constexpr const char* fault_name(Fault f) noexcept {
  switch (f) {
    case Fault::Ok: return "ok";
    case Fault::HexChar: return "bad character";
    case Fault::HexRecord: return "bad record length";
    case Fault::HexDigits: return "odd number of hex digits";
    case Fault::HexChecksum: return "record checksum";
    case Fault::HexType: return "unknown record type";
    case Fault::HexStart: return "start-address record";
    case Fault::HexAfterEof: return "data after the EOF record";
    case Fault::HexNoEof: return "no EOF record";
    case Fault::HexOrder: return "address not the next one";
    case Fault::HexRange: return "address beyond 1 MiB";
    case Fault::HexShort: return "image shorter than 0x410";
    case Fault::BadSp: return "initial SP is not 0x20030000";
    case Fault::BadReset: return "bad reset vector";
    case Fault::BadVector: return "bad vector table";
    case Fault::BadConfig: return "flash configuration field is not the required one";
    case Fault::Idle: return "idle timeout";
    case Fault::Crc: return "crc mismatch";
    case Fault::Readback: return "read-back mismatch";
    case Fault::Disabled: return "firmware writer off";
    case Fault::NoCard: return "no card";
    case Fault::WriteProtected: return "write protected";
    case Fault::NoDir: return "no Firmware/ folder";
    case Fault::BadName: return "name refused";
    case Fault::Exists: return "already exists";
    case Fault::NotFound: return "not found";
    case Fault::Busy: return "busy";
    case Fault::CardBusy: return "busy: the card is in use (a project is loading or a save is running), try again";
    case Fault::NoSpace: return "no space";
    case Fault::Io: return "i/o error";
    case Fault::Unsupported: return "unsupported";
    case Fault::LastPtf: return "last .ptf of Firmware/";
    case Fault::OnHost: return "card on host";
    case Fault::NotArmed: return "not checked, or the card changed since";
    case Fault::Expired: return "check expired, check again";
    case Fault::NameLong: return "name over 30 characters";
    case Fault::NotPadded: return "image not padded: may not boot";
    case Fault::Count: break;
  }
  return "?";
}

}  // namespace o3os::platform::mk66::update
