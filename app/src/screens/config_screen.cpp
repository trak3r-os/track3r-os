// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/config_screen.h>

#include "../sample_text.h"

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/text_format.h>
#include <o3os/model/params.h>
#include <o3os/ui/components.h>

#include <cstring>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr unsigned kPageMidi = 0;
constexpr unsigned kPageDisplay = 1;
constexpr unsigned kPageSd = 2;
constexpr unsigned kPageInfo = kConfigPageInfo;
constexpr unsigned kPageUsb = 4;  // OS-USBRET: the last page, so the numbers of the others stay
constexpr unsigned kPagePattern = 5;  // OS-LIVEREC
constexpr unsigned kPages = 6;

constexpr std::uint16_t kFieldDisk = 0xFFF0;  // OS-USBDISK U2b: the two rows of the USB page that are not settings (a field no descriptor has)
constexpr std::uint16_t kFieldReadOnly = 0xFFF1;
bool virtual_field(std::uint16_t field) noexcept { return field == kFieldDisk || field == kFieldReadOnly; }

struct Rows {
  std::uint16_t field[ParamView::kConfigRows] = {};
  unsigned n = 0;
};

unsigned page_of(const ViewState& view) noexcept {
  const unsigned p = view.page[screen_index(ScreenId::Config)];
  return p < kPages ? p : 0u;
}

// The parameter rows of the page (none on SD and INFO). A page's rows are the device group of the same number, except USB, which is group 2, and PATTERN, group 3.
Rows rows_of(const ViewState& view) noexcept {
  Rows r;
  const unsigned page = page_of(view);
  if (page == kPageMidi || page == kPageDisplay) {
    r.n = model::param_group_fields(core::ParamKind::Device, page, 0, r.field, ParamView::kConfigRows);
  } else if (page == kPageUsb || page == kPagePattern) {
    r.n = model::param_group_fields(core::ParamKind::Device, page == kPageUsb ? 2u : 3u, 0, r.field, ParamView::kConfigRows);
  }
  if (page == kPagePattern || page == kPageMidi) {  // OS-METRO, OS-RECMICRO, OS-PADOFFSET: the order of the spec (REC POLY, REC QUANTIZE, REC MICRO, REC VELOCITY, METRO, METRO VOL, COUNT-IN, PAD OFFSET), not the table's: METRO VOL is row 2, from before the page existed
    // E-CFG-03: the MIDI page puts TRANSPORT IN and OUT (ids 23, 24, appended) right after CLOCK MODE
    namespace ds = model::device_setting;
    static constexpr std::uint16_t kPatternOrder[] = {ds::RecPoly, ds::RecQuantize, ds::RecMicro, ds::RecVelocity, ds::MetroMode, ds::MetronomeVolume, ds::CountIn, ds::PadOffset, ds::PatSwitch};
    static constexpr std::uint16_t kMidiOrder[] = {ds::ClockIn, ds::ClockSync, ds::ClockMode, ds::TransportIn, ds::TransportOut, ds::MidiInPort, ds::MidiOutPort, ds::MidiInChannel, ds::MidiOutChannel};
    const std::uint16_t* const kOrder = page == kPagePattern ? kPatternOrder : kMidiOrder;
    const unsigned order_n = page == kPagePattern ? sizeof kPatternOrder / sizeof kPatternOrder[0] : sizeof kMidiOrder / sizeof kMidiOrder[0];
    const auto rank = [kOrder, order_n](std::uint16_t field) {
      unsigned k = 0;
      while (k < order_n && kOrder[k] != field) {
        ++k;
      }
      return k;  // a row the list does not name goes last, in table order (the insertion sort is stable)
    };
    for (unsigned i = 1; i < r.n; ++i) {
      const std::uint16_t field = r.field[i];
      unsigned j = i;
      for (; j > 0 && rank(r.field[j - 1]) > rank(field); --j) {
        r.field[j] = r.field[j - 1];
      }
      r.field[j] = field;
    }
  }
  if (page == kPageUsb) {  // OS-USBAUDIO: USB AUDIO was appended last (row 17) and is the first row of the page, above USB RETURN
    for (unsigned i = 1; i < r.n; ++i) {
      if (r.field[i] == model::device_setting::UsbAudio) {
        const std::uint16_t audio = r.field[i];
        for (; i > 0; --i) {
          r.field[i] = r.field[i - 1];
        }
        r.field[0] = audio;
        break;
      }
    }
  }
  if (page == kPageUsb && r.n + 2u <= static_cast<unsigned>(ParamView::kConfigRows)) {
    r.field[r.n++] = kFieldDisk;
    r.field[r.n++] = kFieldReadOnly;
  }
  return r;
}

unsigned row_of(const ViewState& view, const Rows& rows) noexcept {
  const unsigned row = view.params.row < 0 ? 0u : static_cast<unsigned>(view.params.row);
  return rows.n == 0 ? 0u : (row >= rows.n ? rows.n - 1u : row);
}

core::ParamId id_of(unsigned field) noexcept { return virtual_field(static_cast<std::uint16_t>(field)) ? core::ParamId::None : core::param_id(core::ParamKind::Device, 0, field); }

// OUT MODE (the Mixer's, the project's) is Stereo, the default: CUE has no output, so a click set to CUE plays on MAIN (OS-METRO D1). A bare Model has no mixer: Stereo.
bool stereo_out(const Model& model) noexcept { return model.mix == nullptr || model.mix->out_mode == 0u; }

struct Licence {
  const char* what;
  const char* licence;
};
constexpr Licence kLicences[] = {
    {"OS CORE", "GPL-3.0-or-later (ADR 0005)"},
    {"MODULE SDK", "MIT: modules may use any licence"},
    {"SPLEEN FONT", "BSD-2-Clause, F. Cambus"},
    {"TERMINUS FONT", "SIL OFL 1.1, D. Zhekov"},
    {"FATFS", "ChaN licence (BSD-style)"},
    {"OPEN303 TB-303", "MIT, R. Schmidt (built in; mod notice)"},
};

}  // namespace

ConfigRow config_row(const ViewState& view) noexcept {
  const Rows rows = rows_of(view);
  const std::uint16_t field = rows.n == 0 ? std::uint16_t{0} : rows.field[row_of(view, rows)];
  return field == kFieldDisk ? ConfigRow::CardAsDisk : (field == kFieldReadOnly ? ConfigRow::ReadOnly : ConfigRow::Param);
}

bool config_page_has_rows(const ViewState& view) noexcept { return rows_of(view).n != 0u; }

void ConfigScreen::enter(const Model&, ViewState& view) const noexcept {
  const Rows rows = rows_of(view);
  view.params.row = static_cast<int>(row_of(view, rows));
}

int ConfigScreen::count(const Model&, const ViewState& view) const noexcept { return static_cast<int>(rows_of(view).n); }
int ConfigScreen::position(const ViewState& view) const noexcept { return static_cast<int>(row_of(view, rows_of(view))); }

bool ConfigScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  (void)dx;  // OS-CFG-ARROWS: Left / Right edit the row (param.edit); the pages are the F-keys and the screen button
  return dy != 0 && set_position(model, view, position(view) + dy);
}

bool ConfigScreen::set_position(const Model&, ViewState& view, int position) const noexcept {
  const int last = static_cast<int>(rows_of(view).n) - 1;
  const int to = last < 0 ? 0 : (position < 0 ? 0 : (position > last ? last : position));
  const bool changed = to != view.params.row;
  view.params.row = to;
  if (changed) {
    view.params.armed = false;  // OS-USBAUDIO: a move of the cursor takes back the first press of APPLY
    view.params.defaults_armed = false;  // OS-SHIFTROW: and the first press of Shift + F7 (the page's DEFAULTS)
  }
  return changed;
}

core::ParamId ConfigScreen::focus_param(const Model&, const ViewState& view) const noexcept {
  const Rows rows = rows_of(view);
  return rows.n == 0 ? core::ParamId::None : id_of(rows.field[row_of(view, rows)]);
}

void ConfigScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kFrameRows);
  const PatternLayout grid = make_pattern_layout(t, kFrameRows, PatternView::kVisibleTracks);
  const unsigned page = page_of(view);
  const Rows rows = rows_of(view);
  const unsigned cursor = row_of(view, rows);
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int name_x = m.index_w + 2 * m.gap;
  const int value_x = name_x + (static_cast<int>(core::kParamNameChars) + 2) * cell_w + 2 * m.gap;
  const int card_x = value_x + 16 * cell_w + 3 * m.gap;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](unsigned i) { return f.pane.y + static_cast<int>(i) * f.row_pitch; };
  const auto cell_y = [&](unsigned i) { return row_y(i) + grid.cell_dy; };
  static constexpr const char* kPageNames[kPages] = {"MIDI", "DISPLAY", "SD", "INFO", "USB", "PATTERN"};

  draw_chrome_bands(r, f, covered);

  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    strip.row().put(Face::Display, "CONFIG", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    strip.row().field("PAGE", Face::Data, kPageNames[page], Color::ChromeDim, Color::ChromeText);
    strip.cpu(model.load);
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    int x = name_x;
    for (unsigned p = 0; p < kPages; ++p) {
      r.text(x, cap_y, kPageNames[p], p == page ? Color::ChromeText : Color::ChromeDim, Face::Label);
      if (p == page) {
        r.panel({x, f.header.y + f.header.h - 2 * m.border, r.text_width(kPageNames[p], Face::Label), 2 * m.border}, Color::Sel);
      }
      x += r.text_width(kPageNames[p], Face::Label) + 4 * m.gap;
    }
    const char* note = "settings are saved to the card";
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(note, Face::Label), cap_y, note, Color::ChromeDim, Face::Label);
  }

  if (page == kPageMidi || page == kPageDisplay || page == kPageUsb || page == kPagePattern) {
    // The selected row: the one strong highlight; the rows, a name and its value; the card on the right says what the descriptor knows.
    const Rect bar = {0, row_y(cursor), card_x - 2 * m.gap, f.row_pitch};  // (the card on the right is not under it)
    if (!hidden(bar)) {
      r.highlight_row(bar);
    }
    char text[core::kParamTextChars + 1];
    for (unsigned i = 0; i < rows.n; ++i) {
      if (virtual_field(rows.field[i])) {  // OS-USBDISK U2b: CARD AS DISK (an action: its value says what F7 does now) and READ ONLY (this session's choice)
        if (!hidden({name_x, cell_y(i), 40 * cell_w, cell_h})) {
          const bool sel_row = i == cursor;
          const bool disk = rows.field[i] == kFieldDisk;
          const char* value = !disk ? (config_read_only_shown(view) ? "On" : "Off")
                                    : (view.disk.phase == ViewState::DiskView::Phase::Out ? "OUT: F7 EJECTS" : (view.disk.phase == ViewState::DiskView::Phase::Working ? "WORKING..." : "F7: SHARE"));
          r.text(name_x, cell_y(i), disk ? "CARD AS DISK" : "READ ONLY", sel_row ? Color::SelText : Color::TextDim);
          r.text(value_x, cell_y(i), value, sel_row ? Color::SelText : Color::Text);
        }
        continue;
      }
      const core::ParamDesc* d = model::param_desc(id_of(rows.field[i]));
      if (d == nullptr || hidden({name_x, cell_y(i), 40 * cell_w, cell_h})) {
        continue;
      }
      const bool sel = i == cursor;
      // OS-USBAUDIO: a row that waits to be applied shows the chosen value, starred and in the accent colour: the unit does what the setting says until APPLY.
      const PendingParam* pending = view.params.find_pending(id_of(rows.field[i]));
      (void)core::param_format(*d, pending != nullptr ? pending->value : model.device.get(rows.field[i]), text, sizeof text);
      if (rows.field[i] == model::device_setting::MetroMode && model.device.get(rows.field[i]) == 1.0f && stereo_out(model)) {
        std::strcpy(text, "Cue (-> Main)");  // OS-METRO D1: OUT MODE Stereo has no CUE output, the click plays on MAIN
      }
      if (const std::size_t len = std::strlen(text); pending != nullptr && len < core::kParamTextChars) {
        text[len] = '*';
        text[len + 1] = '\0';
      }
      r.text(name_x, cell_y(i), d->name, sel ? Color::SelText : Color::TextDim);
      if (rows.field[i] == model::device_setting::UsbReturn && text[0] == 'O' && text[1] == 'n' && mixer::stem_rows(model) >= 10u) {
        // OS-USBOUTS O3: the project's profile has 10 or more channels, so the return is off whatever the setting says (the setting itself is not touched).
        r.text(value_x, cell_y(i), pending != nullptr ? "On (10 OUT: off)*" : "On (10 OUT: off)", sel ? Color::SelText : Color::TextDim);
        continue;
      }
      r.text(value_x, cell_y(i), text, sel ? Color::SelText : (pending != nullptr ? Color::Accent : Color::Text));
      if (d->unit_text[0] != '\0') {
        r.text(value_x + static_cast<int>(std::strlen(text) + 1u) * cell_w, cell_y(i), d->unit_text, sel ? Color::SelText : Color::TextDim);
      }
    }
    if (rows.n != 0u && card_x + 18 * cell_w <= ui::kScreenW && virtual_field(rows.field[cursor]) && !hidden({card_x, row_y(0), ui::kScreenW - card_x, 6 * f.row_pitch})) {
      const bool disk = rows.field[cursor] == kFieldDisk;  // what the row does, in the card on the right
      r.text(card_x, cell_y(0), disk ? "CARD AS DISK" : "READ ONLY", Color::Text);
      r.text(card_x, cell_y(1), disk ? "F7 twice: the computer gets the card" : "On: the computer can only read it", Color::TextDim, Face::Label);
      r.text(card_x, cell_y(2), disk ? "Saves and loads stop meanwhile" : "Off: it can write to the card too", Color::TextDim, Face::Label);
      r.text(card_x, cell_y(3), disk ? "Playing and recording go on" : "Chosen here, for this session only", Color::TextDim, Face::Label);
      if (disk) {
        r.text(card_x, cell_y(4), view.disk.audio ? "USB AUDIO pauses meanwhile" : "Eject: F7 twice, or on the computer", Color::TextDim, Face::Label);
      }
    } else if (rows.n != 0u && card_x + 18 * cell_w <= ui::kScreenW) {
      const unsigned field = rows.field[cursor];
      const core::ParamDesc* d = model::param_desc(id_of(field));
      if (d != nullptr && !hidden({card_x, row_y(0), ui::kScreenW - card_x, 6 * f.row_pitch})) {
        char def[core::kParamTextChars + 1];
        (void)core::param_format(*d, d->def, def, sizeof def);
        core::FixedString<24> line;
        line.assign("DEFAULT ");
        line.append(def);
        r.text(card_x, cell_y(0), d->name, Color::Text);
        r.text(card_x, cell_y(1), line.c_str(), Color::TextDim, Face::Label);
        r.text(card_x, cell_y(2), "Delete: default", Color::TextDim, Face::Label);
        if (field == model::device_setting::UsbReturn) {  // USB RETURN: what the choice means (it is not learnable, so no controller line)
          r.text(card_x, cell_y(3), "Off: capture only, no return", Color::TextDim, Face::Label);
          r.text(card_x, cell_y(4), "On: + stereo return (2 IN)", Color::TextDim, Face::Label);
          r.text(card_x, cell_y(5), "F7 twice: USB reconnects", Color::TextDim, Face::Label);
        } else if (field == model::device_setting::UsbAudio) {  // OS-USBAUDIO: the mode the unit comes up in (not learnable; press to apply)
          r.text(card_x, cell_y(3), "Off: MIDI + serial only", Color::TextDim, Face::Label);
          r.text(card_x, cell_y(4), "On: the audio device too", Color::TextDim, Face::Label);
          r.text(card_x, cell_y(5), "F7 twice: USB reconnects", Color::TextDim, Face::Label);
        } else if (field == model::device_setting::UsbReturnLevel) {  // OS-006e: the host's stream to the outputs (learnable, like the others)
          r.text(card_x, cell_y(3), "FX1 + FX2: bind a controller", Color::TextDim, Face::Label);
          r.text(card_x, cell_y(4), "Host stream to the outputs", Color::TextDim, Face::Label);
          r.text(card_x, cell_y(5), "100 = as sent; not captured", Color::TextDim, Face::Label);
        } else {
          r.text(card_x, cell_y(3), "FX1 + FX2: bind a controller", Color::TextDim, Face::Label);
          if (field == model::device_setting::RecPoly) {  // what the choice means
            r.text(card_x, cell_y(4), "Off: the cursor's track", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "Spill: chords go on to the next", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::RecQuantize) {
            r.text(card_x, cell_y(4), "Nearest: the step nearest the hit", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "Playing: the step now playing", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::RecMicro) {  // OS-RECMICRO
            r.text(card_x, cell_y(4), "On: late notes get m02 in FX2", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "Early ones snap to the step", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::RecVelocity) {
            r.text(card_x, cell_y(4), "External: keyboard velocity", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "goes to the volume column", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::MetroMode) {  // OS-METRO
            r.text(card_x, cell_y(4), "Cue: right jack in MAIN L/CUE R", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "Stereo out: Cue plays on Main", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::MetronomeVolume) {
            r.text(card_x, cell_y(4), "Never in a take, USB or export", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::CountIn) {
            r.text(card_x, cell_y(4), "Bars before a take starts", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "Play in Edit; F1 on Record", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::PadOffset) {  // OS-PADOFFSET
            r.text(card_x, cell_y(4), "12: rows follow on, no overlap", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "Less: rows overlap by 12 - N", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::TransportIn) {  // E-CFG-03
            r.text(card_x, cell_y(4), "Off: ignore Start, Stop, SPP", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "The clock is still followed", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::TransportOut) {
            r.text(card_x, cell_y(4), "Off: send no Start, Stop, SPP", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "The clock still goes out", Color::TextDim, Face::Label);
          } else if (field == model::device_setting::PatSwitch) {  // E-PAT-35
            r.text(card_x, cell_y(4), "Step: a pick while playing starts", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(5), "at the next step", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(6), "Pattern end: it waits for the end", Color::TextDim, Face::Label);
            r.text(card_x, cell_y(7), "of the pattern that plays", Color::TextDim, Face::Label);
          }
        }
      }
    }
  } else if (page == kPageSd) {
    const hal::storage::Storage* card = model.card;
    core::FixedString<32> v;
    const auto line = [&](unsigned i, const char* name, const char* value, Color c) {
      if (!hidden({name_x, cell_y(i), 50 * cell_w, cell_h})) {
        r.text(name_x, cell_y(i), name, Color::TextDim);
        r.text(value_x, cell_y(i), value, c);
      }
    };
    if (card == nullptr) {
      line(0, "CARD", "NO CARD LAYER", Color::TextDim);
    } else {
      const bool present = card->card_present();
      const bool mounted = card->mounted();
      line(0, "CARD", present ? "PRESENT" : "NONE", present ? Color::Text : Color::Accent);
      line(1, "MOUNTED", mounted ? "YES" : "NO", mounted ? Color::Text : Color::TextDim);
      if (mounted) {
        const hal::storage::VolumeInfo vol = card->volume();
        line(2, "FILE SYSTEM", hal::storage::fs_kind_name(vol.kind), Color::Text);
        v.clear();
        append_uint(v, vol.capacity_mb);
        v.append(" MB");
        line(3, "CAPACITY", vol.capacity_mb != 0u ? v.c_str() : "--", Color::Text);
        v.clear();
        append_uint(v, vol.cluster_bytes);
        v.append(" bytes");
        line(4, "CLUSTER", vol.cluster_bytes != 0u ? v.c_str() : "--", Color::Text);
      }
    }
  } else {
    core::FixedString<40> v;
    const auto line = [&](unsigned i, const char* name, const char* value, Color c) {
      if (!hidden({name_x, cell_y(i), 50 * cell_w, cell_h})) {
        r.text(name_x, cell_y(i), name, Color::TextDim);
        r.text(value_x, cell_y(i), value, c);
      }
    };
    line(0, "PRODUCT", model.product[0] != '\0' ? model.product : "--", Color::Text);
    line(1, "BUILD ID", model.build_id[0] != '\0' ? model.build_id : "--", Color::Text);
    if (model.samples != nullptr) {
      append_free_kb(v, model.samples->free_samples());  // OS-AUDIT-A3: KB rounded down, as the sample screens show it
      line(2, "SAMPLE MEMORY", v.c_str(), Color::Text);
    }
    line(3, "CREATED BY", "trak3r-os", Color::Text);
    if (!hidden({name_x, cell_y(4), 40 * cell_w, cell_h})) {
      r.text(name_x, cell_y(4), "LICENSES", Color::Text);
      r.panel({name_x, cell_y(4) + cell_h + 1, 8 * cell_w, m.border}, Color::Sel);
    }
    for (unsigned i = 0; i < sizeof(kLicences) / sizeof(kLicences[0]); ++i) {
      line(5 + i, kLicences[i].what, kLicences[i].licence, Color::Text);
    }
  }

  if (!hidden(f.info)) {
    // USB: the first press of APPLY says what happens (the second applies), a starred value says it waits for it.
    AskText armed_note;  // OS-KEYROWDLG: the armed APPLY's line names its key from the Apply question's table ("USB WILL RECONNECT: PRESS F7 AGAIN", "PRESS F7 AGAIN TO APPLY")
    if (view.params.armed) {
      const bool reconnects = view.params.will_reenumerate(model.device);
      ask_text(Ask::Apply, reconnects ? "USB WILL RECONNECT: PRESS" : "PRESS", armed_note);
      if (!reconnects) {
        armed_note.append(" TO APPLY");
      }
    }
    const char* usb_note = view.params.armed ? armed_note.c_str() : (view.params.pending_n != 0u ? "* chosen, not applied yet: F7" : "Off: 10 OUT. On + profile 8: 8 OUT / 2 IN");
    const ConfigRow special = config_row(view);
    const char* usb_left = special == ConfigRow::CardAsDisk ? "F7 twice: the computer gets the card (saves and loads stop)" : (special == ConfigRow::ReadOnly ? "encoder: On = the computer only reads the card" : usb_note);
    const char* left = page == kPageMidi ? "stored; the clock and router read them later"
                                         : (page == kPageDisplay ? "a change applies at once"
                                                                 : (page == kPageUsb ? usb_left
                                                                                     : (page == kPagePattern ? "live recording, metronome and pads"
                                                                                                             : (page == kPageSd ? "read only" : "ADR 0005: the core is GPL, the module SDK MIT"))));
    draw_info_line(r, f, left, rows.n != 0u ? "up / down: row   encoder: change   Delete: default" : "F1..F6: page");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    const char* const* row = config_key_row(model.firmware, page);  // OS-FWCONFIG: F8 UPDATE on INFO where a firmware update path exists
    const char* keys[8];  // F7 says what it does on the row under the cursor (SHARE / EJECT on CARD AS DISK)
    for (unsigned i = 0; i < 8; ++i) {
      keys[i] = row[i];
    }
    keys[6] = config_f7_label(view);
    if (page == kPageInfo || page == kPageSd) {  // USA-004 (OS-QAFIX-7: and F7 does nothing and says nothing there: param_apply): no row, nothing pending: APPLY would label a key that does nothing
      keys[6] = "";
    }
    draw_question_keys(r, f.keys, *this, model, view, keys);  // (OS-KEYROWDLG: APPLY?, SHARE?, EJECT? and RESET? while armed; OS-FWCONFIG: UPDATE on INFO)
  }
}

constexpr ConfigScreen kConfigScreen{};

}  // namespace o3os::app
