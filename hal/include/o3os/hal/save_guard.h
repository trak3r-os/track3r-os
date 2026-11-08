// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The write guard of the card's save layer (OS-007n): the only paths the OS may create, rename to or remove on the user's card, as pure functions the host
// tests walk exhaustively. The FatFs-backed writer (platform/mk66/save) applies it to the full path right before every call that changes the card, so a bug
// above it cannot reach another file; the save job (app/atomic_save.h) builds its paths with the same helpers.
//
// Paths are relative to the card's root with no leading '/', components separated by '/', FAT case rules (every fixed name below matches in any case):
//
//   Projects                                  folder        make_dir
//   Projects/<name>                           folder        make_dir        <name>: valid_name() (a project folder the user sees in File)
//   Projects/<name>/project.o3pj              Target        the native project file
//   Projects/<name>/project.o3pj.bak          Backup        the previous version of it, kept by a replacing save
//   Projects/<name>/samples                   folder        make_dir        a project's own samples (OS-FU2)
//   Projects/<name>/samples/<stem>.wav        Target        a sample of the project: <stem> valid_name(); replaced by a save of changed audio
//   Projects/<name>/samples/<stem>.wav.bak    Backup        the previous version of it
//   Samples                                   folder
//   Samples/<name>.wav                        Target        a WAV the recorder or the sample editor made: NEVER replaced (no Backup form exists)
//   instruments                               folder        (OS-INSTUI S7) the .pti library: a sample instrument saved on its own, in the OG's instrument format
//   instruments/<name>.pti                    Target        <name> valid_name(); NEVER replaced (no Backup form exists): a name that is taken gets another (app/save_service.h)
//   modules                                   folder        make_dir        (OS-MODPRESET P3) the module folder of the card: only presets/ below it is ours
//   modules/presets                           folder        make_dir
//   modules/presets/<stem>                    folder        make_dir        <stem>: valid_name() (the module file stem, one folder per module)
//   modules/presets/<stem>/<name>.o3mp        Target        <name> valid_name(), at most kPresetNameMax (30) bytes: the rule of tools/o3preset.py valid_file_name; NEVER replaced (no Backup form exists)
//   Settings                                  folder
//   Settings/device.o3st                      Target        the device settings file
//   Settings/device.o3st.bak                  Backup
//   Settings/recent.o3rc                      Target        (OS-FILES part B) the last projects opened or saved, a small text file (app/recent_list.h)
//   Settings/recent.o3rc.bak                  Backup
//   Export                                    folder        the offline WAV exports (OS-WAVEXP)
//   Export/<name>                             folder        make_dir        <name>: valid_name() (the project's name; the official manual puts exports under Export/, official-manual-map)
//   Export/<name>/<stem>.wav                  Target        a rendered song or pattern: <stem> valid_name(); NEVER replaced (no Backup form exists), the name is the next free one
//   Export/<name> OG[ n]                      folder        make_dir        (OS-FILES-C C2a) a whole project in the OG format: <name> OG, then <name> OG 2 .. <name> OG 99 (og_export_folder()); the
//                                                           folder shape is a valid_name like any Export/<name>, so a Temp in it is the stems' rule too
//   Export/<name> OG[ n]/patterns             folder        make_dir        the OG's pattern files
//   Export/<name> OG[ n]/instruments          folder        make_dir        the OG's instrument files
//   Export/<name> OG[ n]/project.mt           Target        the OG project file (3 components); NewOnly, NEVER replaced, no Backup form exists; written LAST
//   Export/<name> OG[ n]/patterns/pattern_NN.mtp   Target  NN 1..255, two digits at least (01..99, then 100..255: the OG's own folders name them so); NEVER replaced
//   Export/<name> OG[ n]/patterns/patternsMetadata Target  the pattern names file; NEVER replaced
//   Export/<name> OG[ n]/instruments/<NN name>.pti Target  NN 01..48, a space, a name: the whole stem a valid_name (<= 31 bytes); NEVER replaced
//   Screenshots                               folder        make_dir        (OS-SCREENSHOT) Shift + Power
//   Screenshots/SHOT_nnnn.bmp                 Target        nnnn 0001..9999 (4 digits); created by the NewOnly rename of the Temp, NEVER replaced, never removed, never renamed (no Backup form exists)
//   AppExport                                 folder        make_dir        (OS-H4) the files the apps write (ADR 0014 amendment A)
//   AppExport/<app>                           folder        make_dir        <app>: valid_name() (the app's file name without .tmod: one folder per app)
//   AppExport/<app>/<stem>.<ext>              Target        a file an app wrote: <stem> valid_name(), <ext> on the allowlist kAppExportExts (mid, syx, txt, csv, json: no .ptf, .tmod, .o3pj or .wav ever comes
//                                                           from an app); created by the NewOnly rename of the Temp, NEVER replaced, never removed, never renamed (no Backup form exists)
//   Projects/<name>/.deleted                  Marker        (OS-FILES part B) DELETE: an empty file, created new (one directory entry, no cluster), never written to, never replaced, never removed; the
//                                                           File list hides a folder that holds it
//   <folder>/_o3save.tmp                      Temp          the file a save is written to before it replaces anything (the file folders above, a project's samples, an export's folder and an app's folder too; in an
//                                                           OG export: the OG folder itself, its patterns/ and its instruments/ and nowhere deeper)
//
// What each operation may touch (the writer enforces it, tests/save_guard_test.cpp walks it):
//   make_dir  a folder name above;
//   create    a Temp only: nothing is ever written to a Target or a Backup directly, a Target appears by a rename;
//   rename    Temp -> Target (the target must not exist), Target -> its own Backup (name + ".bak", same folder), Backup -> its own Target (a restore): in one folder, nothing else. A project folder is
//             NOT renamed (OS-FILES part B dropped it: even a rename inside one folder writes two directory entries, which can sit in different sectors, and a cut between them leaves two names on
//             one chain; tests/save_fatfs_test.cpp, docs/specs/OS-FILES-B.md). Nothing is ever renamed into another folder either.
//   marker    (create_marker) Projects/<name>/.deleted, created new and empty: the one new kind of file, and never anything else.
//   remove    a Temp or a Backup only: never a Target, never a Marker, never a folder, never a file the user made. A Backup is ours (the previous version of a file we wrote).
// An OG export folder is the one place the OS writes the OG's own file names (project.mt, pattern_NN.mtp, patternsMetadata, NN name.pti), and only in the exact shapes above, only below an
// `Export/<name> OG[ n]` folder, never over an existing file and never into a project folder of the user's (Projects/ has no such row). Four components are the limit.
// No call formats, no call removes a folder or a file of the user's, nothing leaves these folders.

#include <cstddef>

namespace o3os::hal::storage::save {

inline constexpr char kProjectsDir[] = "Projects";
inline constexpr char kSamplesDir[] = "Samples";
inline constexpr char kSettingsDir[] = "Settings";
inline constexpr char kInstrumentsDir[] = "instruments";  // OS-INSTUI S7
inline constexpr char kModulesDir[] = "modules";  // OS-MODPRESET P3: modules/presets/<stem>/<name>.o3mp (the .tmod files of modules/ are never touched)
inline constexpr char kPresetsDir[] = "presets";
inline constexpr char kPresetSuffix[] = ".o3mp";
inline constexpr std::size_t kPresetNameMax = 30;  // = FILE_NAME_MAX of tools/o3preset.py: "modules/presets/" + 31 + "/" + 30 + ".o3mp" is 84 bytes
inline constexpr char kExportDir[] = "Export";  // OS-WAVEXP: Export/<project>/<project>_Song0001.wav
inline constexpr char kScreenshotsDir[] = "Screenshots";  // OS-SCREENSHOT: Screenshots/SHOT_0001.bmp (hal/screenshot_file.h)
inline constexpr char kAppExportDir[] = "AppExport";  // OS-H4: AppExport/<app>/<stem>.<ext>, the files an SD app module writes (ADR 0014 amendment A)
// The extensions an app may write, lower case with no dot: the only kinds of file that can come from an app. No firmware, module, project or sample file is among them.
inline constexpr const char* kAppExportExts[] = {"mid", "syx", "txt", "csv", "json"};
inline constexpr std::size_t kAppExportExtCount = sizeof kAppExportExts / sizeof kAppExportExts[0];
inline constexpr char kDeletedMarker[] = ".deleted";  // OS-FILES part B: Projects/<name>/.deleted
inline constexpr char kProjectFile[] = "project.o3pj";
inline constexpr char kProjectSamplesDir[] = "samples";  // Projects/<name>/samples (OS-FU2)
inline constexpr char kSettingsFile[] = "device.o3st";
inline constexpr char kRecentFile[] = "recent.o3rc";  // OS-FILES part B
inline constexpr char kTempName[] = "_o3save.tmp";
inline constexpr char kBackupSuffix[] = ".bak";
inline constexpr char kWavSuffix[] = ".wav";
inline constexpr char kPtiSuffix[] = ".pti";
inline constexpr char kBmpSuffix[] = ".bmp";
inline constexpr char kScreenshotPrefix[] = "SHOT_";
inline constexpr char kProjectMtFile[] = "project.mt";               // OS-FILES-C C2a: the OG project file, in an OG export folder
inline constexpr char kOgPatternsDir[] = "patterns";
inline constexpr char kOgInstrumentsDir[] = "instruments";
inline constexpr char kOgPatternsMetadata[] = "patternsMetadata";
inline constexpr char kOgPatternPrefix[] = "pattern_";
inline constexpr char kOgPatternSuffix[] = ".mtp";
inline constexpr char kOgFolderTag[] = " OG";                       // Export/<name> OG, Export/<name> OG 2 .. 99
inline constexpr unsigned kOgFolderMaxN = 99;
inline constexpr unsigned kOgFolderLastSlot = 48;                     // NN of "NN name.pti": the OG has 48 sample slots
inline constexpr std::size_t kOgNameRoom = 25;                       // bytes of <name> that leave room for " OG 99" inside kNameMax (31)
inline constexpr std::size_t kNameMax = 31;        // bytes of a project folder name or a WAV file's stem
inline constexpr std::size_t kPathCap = 96;        // room for the longest guarded path with its NUL ("Projects/" + 31 + "/samples/" + 31 + ".wav.bak": 88 bytes; "AppExport/" + 31 + "/" + 31 + ".json": 78 bytes;
                                                   // "Export/" + 31 + "/instruments/" + 31 + ".pti": 86 bytes)

enum class Place : unsigned char { None, Folder, Temp, Target, Backup, Marker };

namespace detail {

constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr std::size_t length(const char* s) noexcept {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}
// `s` (n bytes, not NUL-terminated) equals the NUL-terminated `lit` in any case.
constexpr bool same(const char* s, std::size_t n, const char* lit) noexcept {
  std::size_t i = 0;
  for (; i < n && lit[i] != '\0'; ++i) {
    if (lower(s[i]) != lower(lit[i])) {
      return false;
    }
  }
  return i == n && lit[i] == '\0';
}
constexpr bool alnum(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// One component of a path: [begin, begin + n).
struct Part {
  const char* begin;
  std::size_t n;
};

// Splits `path` into at most four components. Returns how many; 0 for an empty path, a leading, trailing or doubled '/', or more than four components.
constexpr unsigned split(const char* path, Part (&out)[4]) noexcept {
  if (path == nullptr || path[0] == '\0') {
    return 0;
  }
  unsigned count = 0;
  const char* p = path;
  for (;;) {
    const char* q = p;
    while (*q != '\0' && *q != '/') {
      ++q;
    }
    if (q == p || count == 4u) {
      return 0;
    }
    out[count++] = Part{p, static_cast<std::size_t>(q - p)};
    if (*q == '\0') {
      return count;
    }
    p = q + 1;
  }
}

// Two components equal in any case.
constexpr bool equal(const Part& a, const Part& b) noexcept {
  if (a.n != b.n) {
    return false;
  }
  for (std::size_t k = 0; k < a.n; ++k) {
    if (lower(a.begin[k]) != lower(b.begin[k])) {
      return false;
    }
  }
  return true;
}

// The stem of a name ends with `suffix` (any case): its length without the suffix, or npos.
constexpr std::size_t stem_of(const Part& name, const char* suffix) noexcept {
  const std::size_t sl = length(suffix);
  if (name.n <= sl || !same(name.begin + name.n - sl, sl, suffix)) {
    return static_cast<std::size_t>(-1);
  }
  return name.n - sl;
}

// A name a user could have typed that this layer will write: 1..kNameMax bytes of letters, digits, space, '_', '-' and '.', the first a letter or digit, the last neither a
// space nor a dot, no "..", and not a name Windows reserves for a device (CON, PRN, AUX, NUL, COM1..9, LPT1..9, with or without an extension).
constexpr bool valid_name(const char* s, std::size_t n) noexcept {
  if (s == nullptr || n == 0 || n > kNameMax || !alnum(s[0]) || s[n - 1] == ' ' || s[n - 1] == '.') {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (!(alnum(c) || c == ' ' || c == '_' || c == '-' || c == '.') || (c == '.' && i + 1 < n && s[i + 1] == '.')) {
      return false;
    }
  }
  std::size_t stem = 0;  // the part before the first dot
  while (stem < n && s[stem] != '.') {
    ++stem;
  }
  if (stem == 3 && (same(s, 3, "con") || same(s, 3, "prn") || same(s, 3, "aux") || same(s, 3, "nul"))) {
    return false;
  }
  if (stem == 4 && s[3] >= '1' && s[3] <= '9' && (same(s, 3, "com") || same(s, 3, "lpt"))) {
    return false;
  }
  return true;
}

// An app's file: "<stem>.<ext>", <stem> a valid_name (at least one byte), <ext> on the allowlist (any case: the names the guard knows match in any case, the OS itself writes them lower case). The extension
// is what follows the LAST dot, so "a.mid.ptf" is a ".ptf" and refused.
constexpr bool app_file_name(const Part& name) noexcept {
  std::size_t dot = name.n;
  while (dot != 0 && name.begin[dot - 1] != '.') {
    --dot;
  }
  if (dot < 2) {  // no dot, or nothing before it
    return false;
  }
  const std::size_t stem = dot - 1;
  const std::size_t ext = name.n - dot;
  bool listed = false;
  for (std::size_t i = 0; i < kAppExportExtCount; ++i) {
    listed = listed || same(name.begin + dot, ext, kAppExportExts[i]);
  }
  return listed && valid_name(name.begin, stem);
}

// OS-FILES-C C2a. The folder of an OG project export: "<name> OG" or "<name> OG n" with n in 2..99 (no leading zero, one space each side of OG and n), <name> at least one byte
// and ending in neither a space nor a dot, the whole a valid_name. Nothing else counts: "<name> OG 1", "OG 100", "OG 0", "OG 02", "OG  2" and a name without the tag are not.
constexpr bool og_folder(const Part& name) noexcept {
  if (!valid_name(name.begin, name.n)) {
    return false;
  }
  std::size_t end = name.n;  // where "<name> OG" ends
  std::size_t last_space = name.n;
  while (last_space != 0 && name.begin[last_space - 1] != ' ') {
    --last_space;
  }
  if (last_space != 0 && last_space < name.n) {  // a trailing " n"?
    const std::size_t digits = name.n - last_space;
    bool all_digits = digits <= 2;
    for (std::size_t i = last_space; i < name.n && all_digits; ++i) {
      all_digits = name.begin[i] >= '0' && name.begin[i] <= '9';
    }
    if (all_digits) {
      const unsigned n = digits == 1 ? static_cast<unsigned>(name.begin[last_space] - '0')
                                     : static_cast<unsigned>(name.begin[last_space] - '0') * 10u + static_cast<unsigned>(name.begin[last_space + 1] - '0');
      if (name.begin[last_space] == '0' || n < 2u || n > kOgFolderMaxN) {
        return false;
      }
      end = last_space - 1;  // (the space before the number)
    }
  }
  const std::size_t tag = length(kOgFolderTag);
  if (end <= tag || !same(name.begin + end - tag, tag, kOgFolderTag)) {
    return false;
  }
  const std::size_t prefix = end - tag;
  return prefix != 0 && name.begin[prefix - 1] != ' ' && name.begin[prefix - 1] != '.';
}

// "pattern_NN.mtp": NN 1..255 written with two digits at least (01..99) or three (100..255), as the OG's own folders name them.
constexpr bool og_pattern_file(const Part& name) noexcept {
  const std::size_t pre = length(kOgPatternPrefix);
  const std::size_t suf = length(kOgPatternSuffix);
  if (name.n < pre + 2 + suf || name.n > pre + 3 + suf || !same(name.begin, pre, kOgPatternPrefix) || !same(name.begin + name.n - suf, suf, kOgPatternSuffix)) {
    return false;
  }
  const std::size_t digits = name.n - pre - suf;
  unsigned v = 0;
  for (std::size_t i = 0; i < digits; ++i) {
    const char c = name.begin[pre + i];
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10u + static_cast<unsigned>(c - '0');
  }
  return v >= 1u && v <= 255u && (digits == 2 || v >= 100u);
}

// "NN name.pti": the stem is a valid_name (31 bytes at most), NN 01..48, a space, then at least one more byte.
constexpr bool og_pti_file(const Part& name) noexcept {
  const std::size_t stem = stem_of(name, kPtiSuffix);
  if (stem == static_cast<std::size_t>(-1) || stem < 4 || !valid_name(name.begin, stem)) {
    return false;
  }
  const char a = name.begin[0];
  const char b = name.begin[1];
  if (a < '0' || a > '9' || b < '0' || b > '9' || name.begin[2] != ' ') {
    return false;
  }
  const unsigned v = static_cast<unsigned>(a - '0') * 10u + static_cast<unsigned>(b - '0');
  return v >= 1u && v <= kOgFolderLastSlot;
}

// OS-SCREENSHOT: "SHOT_nnnn.bmp", nnnn 0001..9999 (four digits).
constexpr bool screenshot_file(const Part& name) noexcept {
  const std::size_t pre = length(kScreenshotPrefix);
  const std::size_t suf = length(kBmpSuffix);
  if (name.n != pre + 4 + suf || !same(name.begin, pre, kScreenshotPrefix) || !same(name.begin + name.n - suf, suf, kBmpSuffix)) {
    return false;
  }
  unsigned v = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const char c = name.begin[pre + i];
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10u + static_cast<unsigned>(c - '0');
  }
  return v >= 1u;
}

}  // namespace detail

constexpr bool valid_name(const char* name) noexcept { return name != nullptr && detail::valid_name(name, detail::length(name)); }

// The name of an OG project export's folder (Export/<this>): "<name> OG" or "<name> OG 2" .. "<name> OG 99" (OS-FILES-C C2a).
constexpr bool og_export_folder(const char* name) noexcept { return name != nullptr && detail::og_folder(detail::Part{name, detail::length(name)}); }

// An extension an app may ask for: exactly one of kAppExportExts, lower case, no dot ("MID", ".mid" and "" are not). The allowlist is the table above and nothing else: classify() reads the same table
// (in any case, for a name that is already on the card), so the export service and the guard cannot disagree about what an app may write.
constexpr bool app_ext_ok(const char* ext) noexcept {
  if (ext == nullptr) {
    return false;
  }
  const std::size_t n = detail::length(ext);
  for (std::size_t i = 0; i < kAppExportExtCount; ++i) {
    const char* const e = kAppExportExts[i];
    if (n != detail::length(e)) {
      continue;
    }
    std::size_t k = 0;
    while (k < n && ext[k] == e[k]) {
      ++k;
    }
    if (k == n) {
      return true;
    }
  }
  return false;
}

// What `path` is to the guard (the table at the top); None for everything else.
constexpr Place classify(const char* path) noexcept {
  detail::Part c[4] = {};
  const unsigned n = detail::split(path, c);
  if (n == 0) {
    return Place::None;
  }
  const bool projects = detail::same(c[0].begin, c[0].n, kProjectsDir);
  const bool samples = detail::same(c[0].begin, c[0].n, kSamplesDir);
  const bool settings = detail::same(c[0].begin, c[0].n, kSettingsDir);
  const bool instruments = detail::same(c[0].begin, c[0].n, kInstrumentsDir);
  const bool exports = detail::same(c[0].begin, c[0].n, kExportDir);
  const bool app_files = detail::same(c[0].begin, c[0].n, kAppExportDir);
  const bool screens = detail::same(c[0].begin, c[0].n, kScreenshotsDir);
  const bool modules = detail::same(c[0].begin, c[0].n, kModulesDir);
  if (!projects && !samples && !settings && !instruments && !exports && !app_files && !screens && !modules) {
    return Place::None;
  }
  if (n == 1) {
    return Place::Folder;
  }
  if (modules) {  // OS-MODPRESET P3: modules/presets/<stem>/<name>.o3mp and nothing else below modules/; created by a rename of the Temp, never replaced
    if (!detail::same(c[1].begin, c[1].n, kPresetsDir)) {
      return Place::None;
    }
    if (n == 2) {
      return Place::Folder;
    }
    if (!detail::valid_name(c[2].begin, c[2].n)) {
      return Place::None;
    }
    if (n == 3) {
      return Place::Folder;
    }
    if (detail::same(c[3].begin, c[3].n, kTempName)) {
      return Place::Temp;
    }
    const std::size_t stem = detail::stem_of(c[3], kPresetSuffix);
    return (stem != static_cast<std::size_t>(-1) && stem <= kPresetNameMax && detail::valid_name(c[3].begin, stem)) ? Place::Target : Place::None;
  }
  if (app_files) {  // OS-H4: AppExport/<app>/<stem>.<ext>, a file an app wrote; created by a rename of the Temp, never replaced, and nothing deeper than the app's folder
    if (!detail::valid_name(c[1].begin, c[1].n)) {
      return Place::None;
    }
    if (n == 2) {
      return Place::Folder;
    }
    if (n != 3) {
      return Place::None;
    }
    if (detail::same(c[2].begin, c[2].n, kTempName)) {
      return Place::Temp;
    }
    return detail::app_file_name(c[2]) ? Place::Target : Place::None;
  }
  if (exports) {  // OS-WAVEXP: Export/<name>/<stem>.wav, a rendered song or pattern; created by a rename of the Temp, never replaced
    if (!detail::valid_name(c[1].begin, c[1].n)) {
      return Place::None;
    }
    if (n == 2) {
      return Place::Folder;
    }
    const bool og = detail::og_folder(c[1]);  // OS-FILES-C C2a: Export/<name> OG[ n], a whole project in the OG format
    if (n == 4) {  // only below an OG folder, in its patterns/ or instruments/, and only the OG's own files
      if (!og) {
        return Place::None;
      }
      const bool in_patterns = detail::same(c[2].begin, c[2].n, kOgPatternsDir);
      if (!in_patterns && !detail::same(c[2].begin, c[2].n, kOgInstrumentsDir)) {
        return Place::None;
      }
      if (detail::same(c[3].begin, c[3].n, kTempName)) {
        return Place::Temp;
      }
      if (in_patterns) {
        return (detail::og_pattern_file(c[3]) || detail::same(c[3].begin, c[3].n, kOgPatternsMetadata)) ? Place::Target : Place::None;
      }
      return detail::og_pti_file(c[3]) ? Place::Target : Place::None;
    }
    if (n != 3) {
      return Place::None;
    }
    if (detail::same(c[2].begin, c[2].n, kTempName)) {
      return Place::Temp;
    }
    if (og) {
      if (detail::same(c[2].begin, c[2].n, kOgPatternsDir) || detail::same(c[2].begin, c[2].n, kOgInstrumentsDir)) {
        return Place::Folder;
      }
      if (detail::same(c[2].begin, c[2].n, kProjectMtFile)) {
        return Place::Target;
      }
    }
    const std::size_t wav = detail::stem_of(c[2], kWavSuffix);
    return (wav != static_cast<std::size_t>(-1) && detail::valid_name(c[2].begin, wav)) ? Place::Target : Place::None;
  }
  if (projects) {
    if (!detail::valid_name(c[1].begin, c[1].n)) {
      return Place::None;
    }
    if (n == 2) {
      return Place::Folder;
    }
    if (detail::same(c[2].begin, c[2].n, kProjectSamplesDir)) {  // OS-FU2: the project's own samples folder and the files in it
      if (n == 3) {
        return Place::Folder;
      }
      if (detail::same(c[3].begin, c[3].n, kTempName)) {
        return Place::Temp;
      }
      const std::size_t wav = detail::stem_of(c[3], kWavSuffix);
      if (wav != static_cast<std::size_t>(-1)) {
        return detail::valid_name(c[3].begin, wav) ? Place::Target : Place::None;
      }
      const std::size_t bak = detail::stem_of(c[3], kBackupSuffix);  // "<stem>.wav.bak"
      if (bak != static_cast<std::size_t>(-1)) {
        const detail::Part target{c[3].begin, bak};
        const std::size_t stem = detail::stem_of(target, kWavSuffix);
        return (stem != static_cast<std::size_t>(-1) && detail::valid_name(c[3].begin, stem)) ? Place::Backup : Place::None;
      }
      return Place::None;
    }
    if (n != 3) {
      return Place::None;
    }
    if (detail::same(c[2].begin, c[2].n, kTempName)) {
      return Place::Temp;
    }
    if (detail::same(c[2].begin, c[2].n, kDeletedMarker)) {
      return Place::Marker;
    }
    if (detail::same(c[2].begin, c[2].n, kProjectFile)) {
      return Place::Target;
    }
    const std::size_t stem = detail::stem_of(c[2], kBackupSuffix);
    return (stem != static_cast<std::size_t>(-1) && detail::same(c[2].begin, stem, kProjectFile)) ? Place::Backup : Place::None;
  }
  if (n != 2) {
    return Place::None;  // Samples, Settings, Screenshots and instruments hold files only
  }
  if (detail::same(c[1].begin, c[1].n, kTempName)) {
    return Place::Temp;
  }
  if (screens) {  // OS-SCREENSHOT: only SHOT_nnnn.bmp, never replaced
    return detail::screenshot_file(c[1]) ? Place::Target : Place::None;
  }
  if (samples || instruments) {  // a file that is never replaced: no Backup form
    const std::size_t stem = detail::stem_of(c[1], samples ? kWavSuffix : kPtiSuffix);
    return (stem != static_cast<std::size_t>(-1) && detail::valid_name(c[1].begin, stem)) ? Place::Target : Place::None;
  }
  if (detail::same(c[1].begin, c[1].n, kSettingsFile) || detail::same(c[1].begin, c[1].n, kRecentFile)) {
    return Place::Target;
  }
  const std::size_t stem = detail::stem_of(c[1], kBackupSuffix);
  return (stem != static_cast<std::size_t>(-1) && (detail::same(c[1].begin, stem, kSettingsFile) || detail::same(c[1].begin, stem, kRecentFile))) ? Place::Backup : Place::None;
}

constexpr bool may_make_dir(const char* path) noexcept { return classify(path) == Place::Folder; }
constexpr bool may_create(const char* path) noexcept { return classify(path) == Place::Temp; }
constexpr bool may_create_marker(const char* path) noexcept { return classify(path) == Place::Marker; }
constexpr bool may_remove(const char* path) noexcept {
  const Place p = classify(path);
  return p == Place::Temp || p == Place::Backup;
}

// rename(from, to): Temp -> Target in one folder; Target -> its own Backup; Backup -> its own Target.
constexpr bool may_rename(const char* from, const char* to) noexcept {
  detail::Part a[4] = {};
  detail::Part b[4] = {};
  const unsigned na = detail::split(from, a);
  const unsigned nb = detail::split(to, b);
  if (na < 2 || na != nb) {
    return false;
  }
  for (unsigned i = 0; i + 1 < na; ++i) {  // the same folder (every component but the last)
    if (!detail::equal(a[i], b[i])) {
      return false;
    }
  }
  const Place pf = classify(from);
  const Place pt = classify(to);
  const detail::Part& fa = a[na - 1];
  const detail::Part& fb = b[nb - 1];
  const auto is_backup_of = [](const detail::Part& backup, const detail::Part& target) {
    return backup.n == target.n + 4 && detail::equal(detail::Part{backup.begin, target.n}, target) &&
           detail::same(backup.begin + target.n, 4, kBackupSuffix);
  };
  if (pf == Place::Temp && pt == Place::Target) {
    return true;
  }
  if (pf == Place::Target && pt == Place::Backup) {
    return is_backup_of(fb, fa);
  }
  if (pf == Place::Backup && pt == Place::Target) {
    return is_backup_of(fa, fb);
  }
  return false;
}

// "a/b/c" into `out` (cap bytes with the NUL); b and c may be null. False when it does not fit (out is then left an empty string).
inline bool join(char* out, std::size_t cap, const char* a, const char* b = nullptr, const char* c = nullptr) noexcept {
  std::size_t n = 0;
  const char* parts[3] = {a, b, c};
  for (unsigned i = 0; i < 3; ++i) {
    if (parts[i] == nullptr) {
      continue;
    }
    if (n != 0) {
      if (n + 1 >= cap) {
        out[0] = '\0';
        return false;
      }
      out[n++] = '/';
    }
    for (const char* p = parts[i]; *p != '\0'; ++p) {
      if (n + 1 >= cap) {
        out[0] = '\0';
        return false;
      }
      out[n++] = *p;
    }
  }
  out[n] = '\0';
  return true;
}

}  // namespace o3os::hal::storage::save
