// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-FLASHDIET I2: what crosses an importer module's boundary (o3m_importer.begin's `options` argument and report()'s `out` buffer),
// in the shapes both sides compile: the module is first-party (kind 3, ABI 1.11) and version-locked by the .tmod's header word 44, so the
// OS and the module are built from this same tree and these structs are the same code on both sides. model/import_layout.h's tag covers
// the model layouts the module writes directly; kImportLayoutVersion there covers a change of these blobs (they hold a pointer and are
// laid out per process, so their raw bytes never enter the tag).
//
// The OS (app::ModuleImport) fills ImportModuleOptions from the File screen's F7 choice and reads ImportModuleReport after step() says
// DONE or FAILED: the REPORT page and the strip are made of it, and the PCM feed asks the module's read_pcm for the slots it names.

#include <o3os/formats/format.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>

#include <cstdint>

namespace o3os::formats {

// The import options the OS hands the importer at begin: the engine choice, as app::ImportEngine (0 auto, 1 amiga, 2 modern). The module
// maps it onto its readers' options, as the in-image loader did.
struct ImportModuleOptions {
  std::uint8_t engine = 0;
  std::uint8_t reserved[3] = {};
};

// One instrument slot of the import, for the PCM feed (read_pcm / frames / rate of the in-image path).
struct ImportModuleSlot {
  std::uint8_t result = 0;  // med::InstResult (Sample: the slot has PCM)
  std::uint8_t reserved[3] = {};
  std::uint32_t frames = 0;  // mono frames of PCM the instrument needs
  std::uint32_t rate = 0;    // Hz; 0: the format's own default rate
};

// The import's report, copied out at o3m_importer.report.
struct ImportModuleReport {
  FormatReport common;  // the reader's FormatReport: error, detail, dropped groups, sanitised
  bool is_med = false;
  bool overflow = false;    // refused for lack of room (Overflow::Refuse)
  bool engine_fx = false;   // the commands went to the Amiga engine window
  std::uint8_t mod_kind = 0;  // mod::Kind: 1 MOD, 2 S3M (0: none, a MED import)
  std::uint8_t channels = 0;  // MOD / S3M channels, MED tracks that carry data
  std::uint16_t patterns = 0;
  std::uint16_t order = 0;
  std::uint16_t tracks_kept = 0;
  std::uint16_t tracks_dropped = 0;
  std::uint16_t instruments = 0;          // made
  std::uint16_t instruments_refused = 0;  // refused or beyond the slots
  std::uint32_t cmd_approx = 0;           // commands written as the nearest thing
  std::uint32_t cmd_dropped = 0;          // commands with no counterpart
  ImportModuleSlot slots[med::caps::kMaxSamples + 1];  // instrument number slot + 1
};

}  // namespace o3os::formats
