// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::app {

// Where the song's commands go when a module is imported (File > IMPORT, F7; ImportOptions::engine_fx of the importers). Auto: what the module is (MOD and MED to the Amiga engine window,
// S3M to the OG / core FX of the Modern engine). Remembered for the session, never saved.
enum class ImportEngine : std::uint8_t { Auto, Amiga, Modern };
inline ImportEngine next_engine(ImportEngine e) noexcept { return e == ImportEngine::Auto ? ImportEngine::Amiga : (e == ImportEngine::Amiga ? ImportEngine::Modern : ImportEngine::Auto); }
inline const char* engine_key_label(ImportEngine e) noexcept { return e == ImportEngine::Amiga ? "FX AMIGA" : (e == ImportEngine::Modern ? "FX MODRN" : "FX AUTO"); }
inline const char* engine_notice(ImportEngine e) noexcept {
  return e == ImportEngine::Amiga ? "IMPORT COMMANDS FOR THE AMIGA ENGINE" : (e == ImportEngine::Modern ? "IMPORT COMMANDS FOR THE MODERN ENGINE" : "IMPORT COMMANDS BY MODULE TYPE");
}

// The File screen's IMPORT page state (OS-FILES part A, docs/specs/OS-FILES-IMPORT.md): the page flag (F6), the browser's folder (the card's path without a leading or trailing '/', "" for the root;
// a folder deeper than that is not entered), the report flag (F8: the last import's report replaces the list), the engine choice (F7), and the request the actions leave for the App's pass,
// which owns the card and the listing (the same split as WavView::go): into the folder under the cursor, or up one.
struct FileImport {
  enum class Go : std::uint8_t { None, Enter, Up };
  static constexpr unsigned kFolderChars = 96;

  bool page = false;
  bool report = false;
  bool all = false;  // OS-IMPORT-ANY I1: the list shows every file (F2 ALL FILES), not only the module names; MODULES at every visit to the page, never saved
  ImportEngine engine = ImportEngine::Auto;
  Go go = Go::None;
  char folder[kFolderChars] = "";
};

}  // namespace o3os::app
