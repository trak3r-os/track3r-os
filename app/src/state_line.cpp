// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/state_line.h>

#include <o3os/app/buttons.h>
#include <o3os/app/dir_scan.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/module_ui.h>
#include <o3os/audio/diag.h>

namespace o3os::app {
namespace {

const char* overlay_name(OverlayId id) noexcept {
  switch (id) {
    case OverlayId::None: return "NONE";
    case OverlayId::Learn: return "LEARN";
    case OverlayId::Navigator: return "NAVIGATOR";
    case OverlayId::Notify: return "NOTIFY";
    case OverlayId::PatternChooser: return "PATTERN_CHOOSER";
    case OverlayId::LengthChooser: return "LENGTH_CHOOSER";
    case OverlayId::JumpChooser: return "JUMP_CHOOSER";
    case OverlayId::ModulePicker: return "MODULE_PICKER";
    case OverlayId::TextEntry: return "TEXT_ENTRY";
    case OverlayId::StepPopup: return "STEP_POPUP";
    case OverlayId::Fill: return "FILL";
    case OverlayId::PlayMode: return "PLAY_MODE";
    case OverlayId::Preset: return "PRESET";
    case OverlayId::VerbMenu: return "VERB_MENU";  // OS-PTOOLS2 S3
    case OverlayId::Events: return "EVENTS";
    case OverlayId::SongTools: return "SONG_TOOLS";
    case OverlayId::PatternTools: return "PATTERN_TOOLS";  // OS-PTOOLS2 S2
    case OverlayId::ExportProgress: return "EXPORT_PROGRESS";
    case OverlayId::FirmwareUpdate: return "FIRMWARE_UPDATE";
    case OverlayId::Help: return "HELP";
  }
  return "?";
}

const char* column_name(Column c) noexcept {
  switch (c) {
    case Column::Note: return "NOTE";
    case Column::Instr: return "INSTR";
    case Column::Vol: return "VOL";
    case Column::Fx1: return "FX1";
    case Column::Fx2: return "FX2";
  }
  return "?";
}

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The name of the directory entry under the cursor of a browser screen (File's PROJECTS and IMPORT pages, Sample load), or "" when the screen has none, no listing is held or the cursor is on no
// entry. A scenario selects a file by name with it (`until`), where a count of Down presses breaks when the card holds one more file (OS-HILNAV). On MODULES it is the file stem of the app under
// the cursor (OS-SMALLFIX6: qa_modules.json walks to an app by name, where a count of Down presses breaks when the card gains an app).
const char* selected_entry(const App& app) noexcept {
  const ViewState& v = app.state();
  if (v.screen == ScreenId::Modules) {
    const auto* entry = app.model().modules != nullptr ? app.model().modules->app_entry_at(v.modules.apps) : nullptr;
    return entry != nullptr ? entry->stem : "";
  }
  const DirScan* scan = app.model().files;
  if (scan == nullptr) {
    return "";
  }
  const DirIndex& index = scan->index();
  int at = -1;
  if (v.screen == ScreenId::SampleLoad) {
    at = v.wav.list.pos();
  } else if (v.screen == ScreenId::File && !v.xport.page) {
    at = v.file.entry(index);
  }
  return at >= 0 && static_cast<unsigned>(at) < index.count() ? index.name(static_cast<unsigned>(at)) : "";
}

}  // namespace

std::size_t describe_state(const App& app, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  const ViewState& v = app.state();
  const ScreenEntry* entry = find_screen(screen_registry(), v.screen);
  const unsigned s = screen_index(v.screen);
  const PatternView& cursor = app.view();
  const audio::PlayPosition::Value& play = v.play;
  std::uint32_t tempo = play.tempo_x100;
  if (!(play.playing && tempo != 0u) && app.model().project != nullptr) {  // as the strip and the screens: the sequencer's tempo while it plays, else the project's (play.tempo_x100 keeps the last played one: a Pattern tempo hold or tap while stopped would lag)
    tempo = app.model().project->tempo_x100();
  }
  t.str("screen=");
  for (const char* n = entry != nullptr ? entry->name : "?"; *n != '\0'; ++n) {
    t.chr(*n == ' ' ? '_' : *n);  // SAMPLE LOAD -> SAMPLE_LOAD: a value has no space
  }
  t.str(" page=").dec(v.page[s]);
  t.str(" mode=").dec(v.mode[s]);
  t.str(" overlay=").str(overlay_name(app.overlay()));
  t.str(" pattern=").dec(v.edit.pattern);
  t.str(" track=").dec(static_cast<std::uint32_t>(cursor.track() < 0 ? 0 : cursor.track()));
  t.str(" row=").dec(static_cast<std::uint32_t>(cursor.row() < 0 ? 0 : cursor.row()));
  t.str(" col=").str(column_name(cursor.column()));
  t.str(" playing=").chr(v.playing ? '1' : '0');
  t.str(" seq=").chr(play.playing ? '1' : '0');
  t.str(" tempo=").dec(tempo);
  t.str(" instrument=").dec(static_cast<std::uint32_t>(v.instrument < 0 ? 0 : v.instrument));
  t.str(" pads=").str(v.screen == ScreenId::Song ? (v.padmode.song_slots() ? "SLOTS" : "TRACKS") : v.padmode.name());  // (OS-SONGPADS: Song has its own pair of modes)
  t.str(" shift=").chr(v.shift ? '1' : '0');
  t.str(" order=");
  if (play.order == audio::PlayPosition::kNoOrder) {
    t.chr('-');
  } else {
    t.dec(play.order);
  }
  t.str(" play_pattern=").dec(play.pattern);
  t.str(" play_step=").dec(play.step);
  t.str(" posts=").dec(v.notify.posts());
  t.str(" sel=");
  const char* sel = selected_entry(app);
  if (*sel == '\0') {
    t.chr('-');
  }
  for (unsigned n = 0; *sel != '\0' && n < 40u; ++sel, ++n) {
    const char c = *sel;
    t.chr(c == ' ' ? '_' : (c > ' ' && c < 0x7F ? c : '?'));
  }
  t.str(" msg=");
  const char* last = v.notify.last_text();
  if (*last == '\0') {
    t.chr('-');
  }
  for (; *last != '\0'; ++last) {
    const char c = *last;
    t.chr(c == ' ' ? '_' : (c > ' ' && c < 0x7F ? c : '?'));
  }
  char keys[kKeysLineChars + 1];  // OS-KEYROWDLG: which keys the prompt in force (a dialog, a question, the boot disk) says, only while one is live
  if (describe_keys(app.model(), v, keys, sizeof keys) != 0u) {
    t.str(" keys=").str(keys);
  }
  return t.size();
}

int button_code(const char* name) noexcept {
  if (name == nullptr || name[0] == '\0') {
    return -1;
  }
  if (name[0] >= '0' && name[0] <= '9') {
    unsigned v = 0;
    for (const char* p = name; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9' || v > 99u) {
        return -1;
      }
      v = v * 10u + static_cast<unsigned>(*p - '0');
    }
    return v < kButtonCount ? static_cast<int>(v) : -1;
  }
  for (const ButtonName& b : kButtonNames) {
    const char* a = name;
    const char* n = b.name;
    while (*a != '\0' && *n != '\0' && lower(*a) == *n) {
      ++a;
      ++n;
    }
    if (*a == '\0' && *n == '\0') {
      return static_cast<int>(b.button);
    }
  }
  return -1;
}

}  // namespace o3os::app
