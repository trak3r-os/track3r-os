// SPDX-License-Identifier: GPL-3.0-or-later
// OS-CEWTENV W8: the user contours of /Envelopes (app/wt_user.h has the rules). Main loop only; read only; no heap.

#include <o3os/app/og_folder.h>
#include <o3os/app/wt_user.h>
#include <o3os/audio/diag.h>
#include <o3os/formats/wav.h>

namespace o3os::app {

namespace {

using hal::storage::DirEntry;
using hal::storage::DirHandle;
using hal::storage::FileHandle;
using hal::storage::Status;
using hal::storage::Storage;

constexpr unsigned kMaxEntries = 4096u;  // directory entries visited per pass: the loop is bounded whatever the folder holds

// A sample below this makes a file bipolar. Not 0: the 24-bit import's TPDF dither (+-1 LSB) would turn a unipolar file that touches 0 into a bipolar one; -8 is -72 dBFS.
constexpr std::int16_t kBipolarFloor = -8;

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

enum class FileResult : std::uint8_t { Loaded, Float, TooLong, Bad };

// Reads one file into mem.pcm through the WAV importer (the left channel of a stereo file) and makes its 128 points into slot `slot`.
FileResult load_file(Storage& storage, const char* path, unsigned slot, WtUserMem& mem) noexcept {
  FileHandle file;
  if (storage.open(path, file) != Status::Ok) {
    return FileResult::Bad;
  }
  StorageSource source;
  source.attach(storage, file);
  formats::wav::Importer importer;
  importer.begin(source, mem.pcm, kWtUserPcmFrames, false, false, 1);
  while (importer.step(formats::wav::kDefaultStepBudget) == formats::wav::Progress::Running) {
  }
  storage.close(file);
  if (importer.progress() != formats::wav::Progress::Done) {
    return importer.report().error == formats::wav::Error::LimitExceeded ? FileResult::TooLong : FileResult::Bad;
  }
  if (source.error() != Status::Ok) {
    return FileResult::Bad;
  }
  const formats::wav::Info& info = importer.info();
  if (info.format == formats::wav::SampleFormat::Float) {
    return FileResult::Float;  // refused: the contour stays the fallback
  }
  if (info.frames == 0 || info.frames > kWtUserPcmFrames) {
    return FileResult::Bad;
  }
  std::int16_t points[audio::kWtContourLen];
  wt_user_make_points(mem.pcm, info.frames, points);
  return audio::wt_user_set(slot, points) ? FileResult::Loaded : FileResult::Bad;
}

}  // namespace

bool wt_user_parse_name(const char* name, unsigned& number) noexcept {
  if (name == nullptr) {
    return false;
  }
  unsigned digits = 0;
  unsigned value = 0;
  while (name[digits] >= '0' && name[digits] <= '9' && digits < 4) {
    value = value * 10u + static_cast<unsigned>(name[digits] - '0');
    ++digits;
  }
  if (digits == 0 || digits > 3) {
    return false;
  }
  const char* ext = name + digits;
  if (ext[0] != '.' || lower(ext[1]) != 'w' || lower(ext[2]) != 'a' || lower(ext[3]) != 'v' || ext[4] != '\0') {
    return false;
  }
  number = value;
  return true;
}

void wt_user_make_points(const std::int16_t* pcm, unsigned frames, std::int16_t* out) noexcept {
  constexpr unsigned kLast = audio::kWtContourLen - 1u;  // 127 intervals
  if (frames == 0) {
    for (unsigned i = 0; i < audio::kWtContourLen; ++i) {
      out[i] = 0;
    }
    return;
  }
  bool bipolar = false;
  for (unsigned i = 0; i < frames; ++i) {
    if (pcm[i] < kBipolarFloor) {
      bipolar = true;
      break;
    }
  }
  const std::uint32_t last = frames - 1u;  // frames <= 2^24: i * last fits 32 bits
  for (unsigned i = 0; i < audio::kWtContourLen; ++i) {
    const std::uint32_t pos = i * last;  // the position in frames is pos / 127
    const std::uint32_t index = pos / kLast;
    const std::int32_t rem = static_cast<std::int32_t>(pos % kLast);
    std::int32_t v = pcm[index];
    if (rem != 0) {  // rem != 0 means index < last: index + 1 exists
      v += (static_cast<std::int32_t>(pcm[index + 1u]) - v) * rem / static_cast<std::int32_t>(kLast);
    }
    if (bipolar) {
      v = (v + 32768) >> 1;  // -1..1 to 0..1
    }
    out[i] = static_cast<std::int16_t>(v < 0 ? 0 : (v > 32767 ? 32767 : v));
  }
}

WtUserReport wt_user_load(Storage& storage, WtUserMem& mem) noexcept {
  WtUserReport report;
  if (!storage.mounted()) {
    return report;  // nothing changes: no card is not an empty card
  }
  std::uint32_t taken[audio::kWtUserCount / 32u] = {};
  DirEntry entry;
  DirHandle dir;
  const Status opened = storage.opendir(kWtUserFolder, dir);
  if (opened == Status::Ok) {
    report.folder = true;
    // Pass 1: the numbering. A 000.wav or 0.wav makes the folder 0-based.
    unsigned seen = 0;
    unsigned number = 0;
    while (seen++ < kMaxEntries && storage.readdir(dir, entry) == Status::Ok) {
      if (!entry.is_dir && wt_user_parse_name(entry.name, number) && number == 0) {
        report.zero_based = true;
        break;
      }
    }
    storage.closedir(dir);
    // Pass 2: the files.
    if (storage.opendir(kWtUserFolder, dir) != Status::Ok) {
      return report;  // a transient card error: leave the contours as they are
    }
    {
      seen = 0;
      Status step = Status::Ok;
      while (seen++ < kMaxEntries && (step = storage.readdir(dir, entry)) == Status::Ok) {
        if (entry.is_dir || !wt_user_parse_name(entry.name, number)) {
          continue;
        }
        const unsigned slot = report.zero_based ? number : number - 1u;  // number 0 only occurs in a 0-based folder
        if (slot >= audio::kWtUserCount) {
          ++report.out_of_range;
          continue;
        }
        if ((taken[slot / 32u] >> (slot % 32u)) & 1u) {
          ++report.duplicate;
          continue;
        }
        if (entry.size > kWtUserMaxFileBytes) {
          ++report.too_long;
          continue;
        }
        char path[40];
        audio::diag::Text(path, sizeof path).str(kWtUserFolder).chr('/').str(entry.name);
        switch (load_file(storage, path, slot, mem)) {
          case FileResult::Loaded:
            taken[slot / 32u] |= 1u << (slot % 32u);
            ++report.loaded;
            break;
          case FileResult::Float: ++report.refused_float; break;
          case FileResult::TooLong: ++report.too_long; break;
          case FileResult::Bad: ++report.bad; break;
        }
      }
      storage.closedir(dir);
      if (step != Status::Ok && step != Status::End) {
        return report;  // the listing failed half way: the files seen so far stay loaded, nothing is reset
      }
    }
  } else if (opened != Status::NotFound && opened != Status::NotADirectory) {
    return report;  // the card failed (not "no such folder"): leave the contours as they are
  }
  // A slot that held a file and has none now goes back to its fallback (an ejected card, another project's card, a deleted file).
  for (unsigned slot = 0; slot < audio::kWtUserCount; ++slot) {
    if (((taken[slot / 32u] >> (slot % 32u)) & 1u) == 0u && audio::wt_user_restore(slot)) {
      ++report.restored;
    }
  }
  return report;
}

std::size_t wt_user_notice(const WtUserReport& report, char* out, std::size_t cap) noexcept {
  if (cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!report.folder || !report.anything()) {
    return 0;
  }
  audio::diag::Text text(out, cap);
  text.str("ENVELOPES: ").dec(report.loaded).str(" LOADED");
  if (report.refused_float != 0) {
    text.str(", ").dec(report.refused_float).str(" FLOAT");
  }
  if (report.skipped() != 0) {
    text.str(", ").dec(report.skipped()).str(" SKIPPED");
  }
  if (report.restored != 0) {
    text.str(", ").dec(report.restored).str(" RESET");
  }
  return text.size();
}

}  // namespace o3os::app
