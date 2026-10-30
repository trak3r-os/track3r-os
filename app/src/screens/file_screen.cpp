// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/file_screen.h>

#include <o3os/app/chrome.h>
#include <o3os/app/dir_scan.h>
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/module_import.h>
#include <o3os/app/og_survey.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/text_format.h>
#include <o3os/ui/components.h>

#include <cstring>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr int kRows = FileView::kVisibleRows;

struct FileLayout {
  Frame frame;
  PatternLayout grid;  // the pattern layout at the same rows: the cell offset, so the lines of the list sit where the Song screen's do
  int marker_x;        // the folder icon, centred in the index column
  int name_x;
  int name_w;
  int tag_x;           // the format of the project (OG, NATIVE), after the name
  bool fits;
};

FileLayout make_file_layout(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  const int cell_w = theme.geometry(Face::Data).cell_w;
  FileLayout l{};
  l.frame = make_frame(theme, kRows);
  l.grid = make_pattern_layout(theme, kRows, PatternView::kVisibleTracks);
  l.marker_x = (m.index_w - static_cast<int>(ui::kIconSize)) / 2;
  l.name_x = m.index_w + 2 * m.gap;
  l.name_w = kFileNameChars * cell_w;
  l.tag_x = l.name_x + l.name_w + 2 * m.gap;
  l.fits = l.name_x + l.name_w <= ui::kScreenW && l.grid.strip.h == l.frame.strip.h && l.grid.info.y == l.frame.info.y;
  return l;
}

// What the listing says when it has no row to show: a headline in the Data face and a line under it in the Label face.
struct Empty {
  const char* headline;
  const char* hint;
};

// The IMPORT page's (OS-FILES): the card from its root, every folder and the module files.
Empty import_empty_state(const DirScan* scan, bool host) noexcept {
  if (host) {
    return {"CARD ON COMPUTER", "CONFIG USB: F7 EJECT"};  // OS-USBDISK 4.3
  }
  if (scan == nullptr) {
    return {"NO SD CARD", "the card is not mounted"};
  }
  switch (scan->state()) {
    case DirScan::State::Idle: return {"NO SD CARD", "the card is not mounted"};
    case DirScan::State::Scanning: return {"SCANNING", ""};
    case DirScan::State::Done: return {"NO MODULES HERE", "folders and .mod .s3m .med .mmd files are listed"};
    case DirScan::State::NoCard: return {"NO SD CARD", "the card is not mounted"};
    case DirScan::State::NoFolder: return {"FOLDER NOT FOUND", "press F3 to go up"};
    case DirScan::State::Failed: return {"CARD ERROR", hal::storage::status_name(scan->error())};
    case DirScan::State::Cancelled: return {"SCAN CANCELLED", "press F2 to read the folder again"};
  }
  return {"", ""};
}

Empty empty_state(const DirScan* scan, bool host) noexcept {
  if (host) {
    return {"CARD ON COMPUTER", "CONFIG USB: F7 EJECT"};  // OS-USBDISK 4.3
  }
  if (scan == nullptr) {
    return {"NO SD CARD", "the card is not mounted"};
  }
  switch (scan->state()) {
    case DirScan::State::Idle: return {"NO SD CARD", "the card is not mounted"};
    case DirScan::State::Scanning: return {"SCANNING", ""};
    case DirScan::State::Done: return {"NO PROJECTS", "a project is a folder with a project.o3pj or a project.mt, inside Projects/"};
    case DirScan::State::NoCard: return {"NO SD CARD", "the card is not mounted"};
    case DirScan::State::NoFolder: return {"NO PROJECTS FOLDER", "make a Projects folder on the card and put the project folders in it"};
    case DirScan::State::Failed: return {"CARD ERROR", hal::storage::status_name(scan->error())};
    case DirScan::State::Cancelled: return {"SCAN CANCELLED", "press F2 to read the folder again"};
  }
  return {"", ""};
}

// The format column: which loader opens the folder.
const char* tag_text(EntryTag tag) noexcept {
  switch (tag) {
    case EntryTag::Og: return "OG";
    case EntryTag::Native: return "NATIVE";
    case EntryTag::Recover: return "RECOVER";
    case EntryTag::None: break;
  }
  return "";
}

// The type column of a module file: by its name, the extension or the prefix ("mod.title"; the importer decides by the file's bytes), and `?` for a file whose name says nothing (ALL FILES mode: a
// listing never reads the contents of a file, OS-IMPORT-ANY I5).
const char* module_tag(const char* name) noexcept {
  switch (module_name_kind(name)) {
    case ModuleName::Mod: return "MOD";
    case ModuleName::S3m: return "S3M";
    case ModuleName::Med: return "MED";
    case ModuleName::None: break;
  }
  return "?";
}

int cell_w_label(const ui::Theme& t) noexcept { return t.geometry(Face::Label).cell_w; }

}  // namespace

namespace {

// The next character of a name as the list draws it: printable ASCII as it is, a control byte or a whole UTF-8 character as one '?'. Moves `p`; false at the end.
bool next_display_char(const unsigned char*& p, char& c) noexcept {
  if (*p == '\0') {
    return false;
  }
  const bool lead = *p >= 0xC0u;
  c = (*p < 0x20u || *p >= 0x7Fu) ? '?' : static_cast<char>(*p);
  ++p;
  if (lead) {
    while ((*p & 0xC0u) == 0x80u) {  // the continuation bytes are part of the one '?'
      ++p;
    }
  }
  return true;
}

}  // namespace

void file_display_name(const char* name, core::FixedString<kFileNameChars>& out, int width) noexcept {
  const int w = width < kFileCutTail + 2 ? kFileCutTail + 2 : (width > kFileNameChars ? kFileNameChars : width);
  char c = 0;
  int total = 0;  // the characters the name has once drawn
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); next_display_char(p, c);) {
    ++total;
  }
  const int head = w - 1 - kFileCutTail;  // OS-AUDIT-37: a name over the width keeps its head and its last kFileCutTail characters, a '~' between
  char text[kFileNameChars + 1];
  int n = 0;
  int i = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); next_display_char(p, c); ++i) {
    if (total <= w || i < head || i >= total - kFileCutTail) {
      text[n++] = c;
    } else if (i == head) {
      text[n++] = '~';
    }
  }
  text[n] = '\0';
  out.assign(text);
}

bool file_is_open_project(const char* project_folder, const char* entry_name) noexcept {
  const std::size_t root = std::strlen(kProjectsFolder);
  return project_folder[0] != '\0' && std::strncmp(project_folder, kProjectsFolder, root) == 0 && project_folder[root] == '/' &&
         std::strcmp(project_folder + root + 1, entry_name) == 0;
}

bool file_sync(const Model& model, ViewState& view) noexcept {
  if (model.files == nullptr) {  // a bare test has no scan: an empty listing
    return view.file.clear();
  }
  return view.file.fit(model.files->index());
}

void FileScreen::enter(const Model& model, ViewState& view) const noexcept {
  view.file.ask_clear();  // F3's question does not wait for the screen to be left and come back to
  (void)file_sync(model, view);
}

Context FileScreen::field_context(const Model&, const ViewState& view) const noexcept { return view.xport.page ? Context::FileExport : Context::None; }

int FileScreen::count(const Model& model, const ViewState& view) const noexcept {
  if (view.xport.page) {
    return static_cast<int>(ExportView::kRows);  // OS-WAVEXP: SONG, PATTERN, SONG STEMS, PATT STEMS; OS-FILES-C C2c: OG PROJECT
  }
  return model.files != nullptr ? view.file.rows(model.files->index()) : 0;  // (the RECENT group's rows, OS-FILES part B, and the listing's)
}

bool FileScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  if (view.xport.page) {
    const std::uint8_t to = position <= 0 ? std::uint8_t{0} : (position >= static_cast<int>(ExportView::kRows) ? static_cast<std::uint8_t>(ExportView::kRows - 1u) : static_cast<std::uint8_t>(position));
    const bool moved = to != view.xport.row;
    view.xport.row = to;
    view.xport.fresh = view.xport.fresh && !moved;  // the file the row would make is looked up again
    if (moved) {
      view.file.ask_clear();  // a move takes back the OG PROJECT row's question (as it does NEW's)
    }
    return moved;
  }
  return model.files != nullptr && view.file.set(position, model.files->index());
}

bool FileScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (view.xport.page) {
    return dx == 0 && dy != 0 && set_position(model, view, static_cast<int>(view.xport.row) + dy);
  }
  if (model.files == nullptr) {
    return false;
  }
  const DirIndex& index = model.files->index();
  if (dy != 0) {
    return dx == 0 && view.file.move(dy, index);
  }
  if (dx == 0) {
    return false;
  }
  const int to = view.file.letter(dx > 0 ? 1 : -1, index);
  return to != DirIndex::kNone && view.file.set(to, index);
}

void FileScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const FileLayout l = make_file_layout(t);
  const Frame& f = l.frame;
  const DirScan* scan = model.files;
  const DirIndex* index = scan != nullptr ? &scan->index() : nullptr;
  const bool exp = view.xport.page;  // OS-WAVEXP: the EXPORT page: two rows in the place of the listing
  FileView fv = view.file;
  if (index != nullptr) {
    (void)fv.fit(*index);  // drawn inside the list whatever the view says: the scan may have added entries since the cursor last moved
  } else {
    fv.reset();
  }
  const int length = exp ? static_cast<int>(ExportView::kRows) : (index != nullptr ? fv.rows(*index) : 0);  // rows: the RECENT group's, then the listing's
  const int items = exp ? length : (index != nullptr ? static_cast<int>(index->count()) : 0);               // projects (or entries): the group repeats some of them
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int visible = exp ? length : (length - fv.top() < kRows ? length - fv.top() : kRows);
  const int cursor_slot = exp ? static_cast<int>(view.xport.row) : fv.pos() - fv.top();
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](int i) { return f.pane.y + i * f.row_pitch; };
  const auto cell_y = [&](int i) { return row_y(i) + l.grid.cell_dy; };
  const bool scanning = scan != nullptr && scan->state() == DirScan::State::Scanning;
  const bool imp = view.import.page && !exp;                                                  // OS-FILES: the IMPORT page
  const bool show_report = imp && view.import.report && model.import_report != nullptr;       // ... and its report instead of the list

  draw_chrome_bands(r, f, covered);
  const Rect row_bar = {0, row_y(cursor_slot < 0 ? 0 : cursor_slot), ui::kScreenW, f.row_pitch};
  if (length > 0 && !show_report && !hidden(row_bar)) {  // an empty list has nothing selected
    r.highlight_row(row_bar);
  }

  // The status strip: the title in the Display face, the transport badge, the folder and the counts in the Data face, CPU at the right edge.
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<24> text;
    row.put(Face::Display, "FILE", Color::ChromeText);
    strip.divider();
    if (view.xport.running) {
      strip.exporting();  // OS-WAVEXP: the transport is stopped for an export; the badge says what the unit is doing
    } else {
      strip.transport(view.playing);
    }
    strip.divider();
    text.assign(exp ? "Export" : imp ? (view.import.folder[0] != '\0' ? view.import.folder : "/") : kProjectsFolder);
    row.field(imp ? "IMPORT" : (exp ? "FOLDER" : "DIR"), Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    append_uint(text, static_cast<unsigned>(items));
    if (scanning) {
      text.append("..");  // more are coming
    } else if (scan != nullptr && scan->truncated()) {
      text.append("+");  // and more than the listing holds
    }
    row.field("ITEMS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    if (length > 0) {
      if (!exp && fv.pos() < fv.lead()) {
        text.assign("R");  // a row of the RECENT group: R1 .. R5
        append_uint(text, static_cast<unsigned>(fv.pos() + 1));
      } else {
        append_uint(text, static_cast<unsigned>(exp ? view.xport.row + 1 : fv.pos() - fv.lead() + 1));
      }
    } else {
      text.assign("--");
    }
    row.field("POS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    strip.cpu(model.load);
  }

  // The header: the column's name and the page at the right.
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.panel({l.name_x - l.grid.cursor_pad_x, f.header.y + f.header.h - 2 * m.border, l.name_w + 2 * l.grid.cursor_pad_x, 2 * m.border}, Color::Sel);
    r.text(l.name_x, cap_y, exp ? "WHAT" : "NAME", Color::ChromeText, Face::Label);
    const char* const page_name = exp ? "EXPORT" : (imp ? "IMPORT" : "OPEN");
    if (!exp) {
      r.text(l.tag_x, cap_y, imp ? "TYPE" : "FORMAT", Color::ChromeText, Face::Label);
    }
    const int page_x = ui::kScreenW - 2 * m.pad - r.text_width(page_name, Face::Label);
    r.text(page_x, cap_y, page_name, Color::ChromeText, Face::Label);
    // OS-NAMES: the open project by its title, or by its folder's name when the title is empty (nothing for a new project with neither), cut to the room between the columns and the page's name.
    const char* title = model.project != nullptr ? model.project->name() : "";
    if (title[0] == '\0') {
      const char* slash = std::strrchr(model.project_folder, '/');
      title = slash != nullptr ? slash + 1 : model.project_folder;
    }
    // OS-AUDIT-A6 (E-FILE-17, new Community OS behaviour): while the project has changes no save has taken, the header says UNSAVED where it says PROJECT (a "*" when the room is short; "UNSAVED" alone for a
    // project with no name), so that the state the questions ask about is also there before any key is pressed.
    const bool unsaved = model.project_unsaved;
    if (title[0] != '\0' || unsaved) {
      const int from = (exp ? l.name_x + r.text_width("WHAT", Face::Label) : l.tag_x + r.text_width("FORMAT", Face::Label)) + 4 * m.gap;
      const int room = (page_x - 4 * m.gap - from) / cell_w_label(t);
      core::FixedString<kFileNameChars> shown;
      file_display_name(title, shown);
      core::FixedString<kFileNameChars + 8> text;
      text.assign(room >= 8 + 8 ? (unsaved ? "UNSAVED " : "PROJECT ") : (unsaved ? "* " : ""));
      if (title[0] == '\0') {
        text.assign(room >= 7 ? "UNSAVED" : "*");
      }
      append_clipped(text, shown.c_str(), static_cast<std::size_t>(room > 0 ? room - static_cast<int>(text.size()) : 0));
      r.text(page_x - 4 * m.gap - r.text_width(text.c_str(), Face::Label), cap_y, text.c_str(), Color::ChromeText, Face::Label);
    }
  }

  // The info line and the key row.
  if (!hidden(f.info)) {
    core::FixedString<64> text;
    if (exp) {
      text.assign(view.xport.running ? "exporting" : (view.xport.row == ExportView::Song ? "export the whole song" : (view.xport.row == ExportView::Pattern ? "export one pattern" : (view.xport.row == ExportView::SongStems ? "export the song as stems" : (view.xport.row == ExportView::PatternStems ? "export one pattern as stems" : "export the project as an OG folder")))));
    } else if (show_report) {
      text.assign("last import   F8: back to the list");
    } else if (length == 0 && !scanning) {
      text.assign((imp ? import_empty_state(scan, view.card_on_computer) : empty_state(scan, view.card_on_computer)).headline);
    } else if (scanning) {
      text.assign("reading ");
      text.append(imp ? (view.import.folder[0] != '\0' ? view.import.folder : "/") : kProjectsFolder);
      text.append("   ");
      append_uint(text, static_cast<unsigned>(items));
      text.append(" so far");
    } else if (scan != nullptr && scan->truncated()) {
      text.assign("list full: the first ");
      append_uint(text, static_cast<unsigned>(items));
      text.append(" are shown");
    } else {
      append_uint(text, static_cast<unsigned>(items));
      text.append(imp ? (items == 1 ? " entry" : " entries") : (items == 1 ? " project" : " projects"));
      if (imp && view.import.all) {
        text.append(", all files");  // OS-IMPORT-ANY I1
      }
    }
    draw_info_line(r, f, text.c_str(), exp ? (view.xport.running ? "F8 cancel" : "F1 export   F6 import   F7 projects") : (imp ? "F1 import   F3 up   F7 fx" : "left / right: letter   F2 rescan   Delete: delete"));
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    const char* keys[8];
    for (int i = 0; i < 8; ++i) {
      keys[i] = exp ? kFileExportSoftKeys[i] : kFileSoftKeys[i];
    }
    if (imp) {  // OS-FILES: the IMPORT page's keys; F4 and F5 are the same SAVE and SAVE AS
      keys[0] = length > 0 && index != nullptr && index->is_dir(static_cast<unsigned>(fv.pos())) ? "ENTER" : "IMPORT";
      keys[1] = view.import.all ? "MODULES" : "ALL";  // OS-IMPORT-ANY I1: the mode a press goes to ("ALL FILES" is one character too wide for a key)
      keys[2] = "UP";
      keys[5] = "PROJECTS";
      keys[6] = engine_key_label(view.import.engine);
      keys[7] = show_report ? "LIST" : "REPORT";
    }
    draw_question_keys(r, f.keys, *this, model, view, keys);  // (OS-KEYROWDLG: the answers of the question in force are marked)
  }

  if (exp) {  // OS-WAVEXP: the rows, then the card of what F1 would make
    // Under the export's progress dialog nothing is drawn that touches it (OS-BUG-OGOVERLAY): these lines run wider than the dialog, and the unit shows them through its fill.
    const auto under = [&](const Rect& rect) { return touches(covered, rect.x, rect.y, rect.w, rect.h); };
    core::FixedString<24> name[ExportView::kRows];
    name[ExportView::Song].assign("SONG");
    name[ExportView::Pattern].assign("PATTERN ");
    append_dec(name[ExportView::Pattern], view.xport.pattern, 2);
    name[ExportView::SongStems].assign("SONG STEMS");
    name[ExportView::PatternStems].assign("PATT STEMS ");
    append_dec(name[ExportView::PatternStems], view.xport.pattern, 2);
    name[ExportView::OgProject].assign("OG PROJECT");
    for (int i = 0; i < length; ++i) {
      if (!under({l.name_x, cell_y(i), l.name_w, cell_h})) {
        r.text(l.name_x, cell_y(i), name[i].c_str(), i == cursor_slot ? Color::SelText : Color::Text);
      }
    }
    // OS-FILES-C C2c: the OG PROJECT row says what it is next to its name, until an official unit has opened an export (docs/specs/OS-FILES-C.md section 8). OS-BUG-LEGIB: on the selected row it is
    // SelText like the rest of that row (TextDim is 1.7:1 on Sel; no theme can make it 4.5:1 on both Sel and Pane and stay dim).
    if (length > static_cast<int>(ExportView::OgProject) && !under({l.name_x, cell_y(ExportView::OgProject), l.name_w, cell_h})) {
      r.text(l.name_x + 12 * t.geometry(Face::Data).cell_w, ui::caps_y(t, Face::Label, cell_y(ExportView::OgProject), cell_h), "EXPERIMENTAL",
             cursor_slot == static_cast<int>(ExportView::OgProject) ? Color::SelText : Color::TextDim, Face::Label);
    }
    const bool og = view.xport.row == ExportView::OgProject;
    const int card_y = cell_y(length + 1);
    static constexpr const char* kKinds[ExportView::kRows] = {"the arrangement, with its tail", "the pattern once, with its tail", "the song as one file per track, the delay, the reverb and the MIX", "the pattern as one file per track, the delay, the reverb and the MIX",
                                                              "the whole project as an OG folder: patterns, instruments, project.mt (target 1.9.0)"};
    const char* const kind = kKinds[view.xport.row < ExportView::kRows ? view.xport.row : 0];
    char file_line[ExportView::kFileChars];
    core::FixedString<48> format_line;
    const char* lines[4] = {view.xport.preview[0] != '\0' ? view.xport.preview : "NO FILE NAME: NO CARD, OR NO FREE NUMBER", kind, "16-bit stereo, 44,100 Hz", ""};
    if (og) {
      std::strcpy(file_line, view.xport.fresh ? "NO FOLDER NAME: NO CARD, NO FREE NAME OR NOTHING TO EXPORT" : "COUNTING...");  // (not fresh: the survey is being made a slice a pass, App::poll_export)
      if (view.xport.og_valid && view.xport.og.folder[0] != '\0') {
        std::strcpy(file_line, "Export/");
        std::size_t n = std::strlen(file_line);
        for (const char* p = view.xport.og.folder; *p != '\0' && n + 1u < sizeof file_line; ++p) {
          file_line[n++] = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
        }
        file_line[n] = '\0';
        const std::uint32_t tenths = (view.xport.og.bytes + 50000u) / 100000u;  // MB of 1,000,000 bytes, in tenths
        format_line.assign("about ");
        if (tenths >= 10u) {
          append_uint(format_line, tenths / 10u);
          format_line.append(".");
          append_uint(format_line, tenths % 10u);
          format_line.append(" MB, ");
        } else {
          append_uint(format_line, (view.xport.og.bytes + 999u) / 1000u);
          format_line.append(" KB, ");
        }
        append_uint(format_line, view.xport.og.files);
        format_line.append(" FILES");
      }
      lines[0] = file_line;
      lines[2] = format_line.c_str();
    }
    const char* const heads[4] = {"FILE", "WHAT", og ? "SIZE" : "FORMAT", ""};
    for (int i = 0; i < 3; ++i) {
      const int y = card_y + i * f.row_pitch;
      if (!under({l.name_x, y, ui::kScreenW - l.name_x, cell_h})) {
        r.text(l.name_x, ui::caps_y(t, Face::Label, y, f.row_pitch), heads[i], Color::TextDim, Face::Label);
        r.text(l.name_x + 8 * cell_w_label(t), ui::caps_y(t, Face::Label, y, f.row_pitch), lines[i], Color::Text, Face::Label);
      }
    }
    static constexpr const char* kNotes[2] = {"Once started, the transport stops, and nothing else works until the export ends.", "While it runs, F8 cancels it; the file being made is not left (stems already made stay)."};
    static constexpr const char* kOgNotes[2] = {"NOT YET OPENED ON AN OFFICIAL UNIT. Once started, nothing else works until it ends.", "While it runs, F8 stops it: finished files stay, and there is no project.mt until the last."};
    for (int i = 0; i < 2; ++i) {
      const int note_y = card_y + (4 + i) * f.row_pitch;
      if (!under({l.name_x, note_y, ui::kScreenW - l.name_x, f.row_pitch})) {
        r.text(l.name_x, ui::caps_y(t, Face::Label, note_y, f.row_pitch), og ? kOgNotes[i] : kNotes[i], Color::TextDim, Face::Label);
      }
    }
    if (og && view.xport.og_valid) {  // what the export would leave out or rewrite (the strip's question says the same shorter)
      OgLossItem kinds[kOgLossMax];
      const unsigned count = og_loss_items(view.xport.og.drops, kinds);
      constexpr unsigned kLossLines = 3;  // (the unit's key row clips a fourth: three lines, then every kind short and wrapped)
      const int x = l.name_x + 8 * cell_w_label(t);
      const int head_y = card_y + 6 * f.row_pitch;
      if (!under({l.name_x, head_y, ui::kScreenW - l.name_x, cell_h})) {
        r.text(l.name_x, ui::caps_y(t, Face::Label, head_y, f.row_pitch), count == 0u ? "KEEPS" : "LOSES", Color::TextDim, Face::Label);
      }
      if (count == 0u && !under({l.name_x, head_y, ui::kScreenW - l.name_x, cell_h})) {
        r.text(x, ui::caps_y(t, Face::Label, head_y, f.row_pitch), "everything the OG format has a place for", Color::Text, Face::Label);
      }
      const auto line_y = [&](unsigned i) { return head_y + static_cast<int>(i) * f.row_pitch; };
      if (count <= kLossLines) {  // up to three kinds: one a line, said in full
        for (unsigned i = 0; i < count; ++i) {
          if (under({x, line_y(i), ui::kScreenW - x, cell_h})) {
            continue;
          }
          core::FixedString<64> text;
          append_uint(text, kinds[i].n);
          text.append(" ");
          text.append(kinds[i].full);
          r.text(x, ui::caps_y(t, Face::Label, line_y(i), f.row_pitch), text.c_str(), Color::Text, Face::Label);
        }
      } else {  // more: EVERY kind, in the short form, run together and wrapped at the card's width (the worst case, all fourteen, fits three lines)
        const unsigned room = static_cast<unsigned>((ui::kScreenW - x - 2 * m.pad) / cell_w_label(t));
        core::FixedString<96> text;
        unsigned row = 0;
        for (unsigned i = 0; i < count; ++i) {
          core::FixedString<32> part;
          append_uint(part, kinds[i].n);
          part.append(" ");
          part.append(kinds[i].brief);
          if (i + 1u < count) {
            part.append(",");
          }
          if (!text.empty() && text.size() + 1u + part.size() > room && row + 1u < kLossLines) {
            if (!under({x, line_y(row), ui::kScreenW - x, cell_h})) {
              r.text(x, ui::caps_y(t, Face::Label, line_y(row), f.row_pitch), text.c_str(), Color::Text, Face::Label);
            }
            text.clear();
            ++row;
          }
          if (!text.empty()) {
            text.append(" ");
          }
          text.append(part.c_str());
        }
        if (!under({x, line_y(row), ui::kScreenW - x, cell_h})) {
          r.text(x, ui::caps_y(t, Face::Label, line_y(row), f.row_pitch), text.c_str(), Color::Text, Face::Label);
        }
      }
    }
    return;
  }
  if (show_report) {  // the last import's report in place of the list
    ModuleReportText text;
    module_report_text(*model.import_report, text);
    for (unsigned i = 0; i < text.count && static_cast<int>(i) < kRows; ++i) {
      if (!hidden({l.name_x, cell_y(static_cast<int>(i)), ui::kScreenW - l.name_x, cell_h})) {
        r.text(l.name_x, cell_y(static_cast<int>(i)), text.line[i], i == 0u ? Color::Text : Color::TextDim);
      }
    }
    return;
  }

  // Nothing to list: say why, in the middle of the pane.
  if (length == 0 || index == nullptr) {
    const Empty e = imp ? import_empty_state(scan, view.card_on_computer) : empty_state(scan, view.card_on_computer);
    const int mid = f.pane.y + f.pane.h / 2;
    const int head_w = r.text_width(e.headline, Face::Data);
    const int head_y = mid - cell_h;
    if (!hidden({(ui::kScreenW - head_w) / 2, head_y, head_w, cell_h})) {
      r.text((ui::kScreenW - head_w) / 2, head_y, e.headline, Color::TextDim, Face::Data);
    }
    const int hint_w = r.text_width(e.hint, Face::Label);
    const int hint_y = ui::caps_y(t, Face::Label, mid + m.pad, f.row_pitch);
    if (e.hint[0] != '\0' && !hidden({(ui::kScreenW - hint_w) / 2, hint_y, hint_w, f.row_pitch})) {
      r.text((ui::kScreenW - hint_w) / 2, hint_y, e.hint, Color::TextDim, Face::Label);
    }
    return;
  }

  // The rows: the names, one colour pass at a time (the selected row is all SelText), a folder icon in front of a folder.
  core::FixedString<kFileNameChars> names[kRows];
  for (int i = 0; i < visible; ++i) {
    file_display_name(index->name(static_cast<unsigned>(fv.entry_at(fv.top() + i, *index))), names[i]);  // (a recent row shows its entry's name; an entry that is gone shows "")
  }
  static constexpr Color kPasses[] = {Color::Text, Color::SelText};
  for (const Color pass : kPasses) {
    for (int i = 0; i < visible; ++i) {
      const unsigned entry = static_cast<unsigned>(fv.entry_at(fv.top() + i, *index));  // (a row of the RECENT group is the same folder as its row in the A-Z list)
      const bool recent = fv.top() + i < fv.lead();
      const bool sel = i == cursor_slot;
      if ((sel ? Color::SelText : Color::Text) != pass) {
        continue;
      }
      const int icon_y = row_y(i) + (f.row_pitch - static_cast<int>(ui::kIconSize)) / 2;
      if (index->is_dir(entry) && !hidden({l.marker_x, icon_y, static_cast<int>(ui::kIconSize), static_cast<int>(ui::kIconSize)})) {
        r.icon(l.marker_x, icon_y, ui::Icon::Folder, pass);
      }
      constexpr int kBullet = 4;  // OS-AUDIT-19: the open project's row: a square bullet in the margin left of the folder icon
      const Rect bullet = {(l.marker_x - kBullet) / 2, row_y(i) + (f.row_pitch - kBullet) / 2, kBullet, kBullet};
      if (!imp && index->is_dir(entry) && file_is_open_project(model.project_folder, index->name(entry)) && !hidden(bullet)) {
        r.panel(bullet, pass);
      }
      if (!hidden({l.name_x, cell_y(i), l.name_w, cell_h})) {
        r.text(l.name_x, cell_y(i), names[i].c_str(), pass);
      }
      const char* const tag = imp ? (index->is_dir(entry) ? "" : module_tag(index->name(entry))) : tag_text(index->tag(entry));
      if (tag[0] != '\0' && !hidden({l.tag_x, cell_y(i), r.text_width(tag, Face::Data), cell_h})) {
        r.text(l.tag_x, cell_y(i), tag, pass);
      }
      if (recent) {  // the group's label, at the right edge
        const int w = r.text_width("RECENT", Face::Label);
        const int x = ui::kScreenW - 2 * m.pad - w;
        if (!hidden({x, ui::caps_y(t, Face::Label, row_y(i), f.row_pitch), w, f.row_pitch})) {
          r.text(x, ui::caps_y(t, Face::Label, row_y(i), f.row_pitch), "RECENT", pass == Color::SelText ? Color::SelText : Color::TextDim, Face::Label);
        }
      }
    }
  }
}

constexpr FileScreen kFileScreen{};

}  // namespace o3os::app
