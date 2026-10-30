// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware update dialog (OS-FWCONFIG slice 2a; firmware_view.h, firmware_actions.cpp): the list of Firmware/*.ptf, the two questions, the check's bar and the hand-over's "do not power off". The
// dialog owns the F-key row (its prompt in key_prompt.cpp, painted by render_scene); its hint line is made from the same table, so no key is named in a string here. Only tokens of the contrast
// table are used: ChromeText and ChromeDim on Chrome, and Chrome knocked out of a ChromeText chip.
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kFirmwareW = 720;
constexpr int kLine = 2;                                         // rows of the message / question area above the list
constexpr int kRows = static_cast<int>(FirmwareView::kRows);
constexpr int kBodyRows = kLine + kRows + 1;                     // the message, the list, the hints

Dialog firmware_dialog(const ui::Theme& theme) noexcept { return make_dialog(theme, kFirmwareW, kBodyRows * theme.metrics.row_pitch); }

using Text = FirmwareView::Text;

// "12345 KB" for a file's size (a .ptf is HEX text of some hundreds of KB).
void append_kib(Text& out, std::uint32_t bytes) noexcept {
  append_uint(out, (bytes + 1023u) / 1024u);
  out.append(" KB");
}

// The rows of the dialog's body: `put` writes text on row `row` from `x`, in the face and ink given (one place that does the vertical centring, instead of one for each line below).
struct Body {
  ui::Renderer& r;
  int x0;  // the first row's top
  int pitch;
  void put(int row, int x, const char* text, Face face, Color ink) const { r.text(x, ui::caps_y(r.theme(), face, x0 + row * pitch, pitch), text, ink, face); }
};

std::uint16_t permille_of(std::uint32_t done, std::uint32_t total) noexcept {
  const std::uint32_t p = ((done >> 8) * 1000u) / ((total >> 8) | 1u);  // (the file is at most 4 MiB: no overflow; a total under 256 B reads as 0)
  return static_cast<std::uint16_t>(p > 1000u ? 1000u : p);
}

}  // namespace

Rect FirmwareOverlay::rect(const ui::Theme& theme) const noexcept { return firmware_dialog(theme).outer; }

// The cursor runs over the whole folder (OS-BUG-FWLIST), not over the window held: count() is the folder's, position() the folder's index.
int FirmwareOverlay::count(const Model&, const ViewState& view) const noexcept { return view.firmware.phase == FirmwareView::Phase::List ? static_cast<int>(view.firmware.entries) : 0; }
int FirmwareOverlay::position(const ViewState& view) const noexcept { return static_cast<int>(view.firmware.position()); }
int FirmwareOverlay::page() const noexcept { return kRows; }

// A position inside the window moves the cursor; one outside records where it goes (`want`) and the App's next pass loads that window (this call has no sink). Several moves in one pass add up.
bool FirmwareOverlay::set_position(const Model&, ViewState& view, int position) const noexcept {
  FirmwareView& v = view.firmware;
  if (v.phase != FirmwareView::Phase::List || v.entries == 0) {
    return false;
  }
  const unsigned to = position < 0 ? 0u : (position >= static_cast<int>(v.entries) ? v.entries - 1u : static_cast<unsigned>(position));
  if (to == v.position()) {
    return false;
  }
  if (to >= v.first && to < static_cast<unsigned>(v.first) + v.n) {
    v.cursor = static_cast<std::uint8_t>(to - v.first);
    v.want = FirmwareView::kNoWant;
  } else {
    v.want = static_cast<std::uint16_t>(to);
  }
  return true;
}

void FirmwareOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = firmware_dialog(t);
  const FirmwareView& v = view.firmware;
  using Phase = FirmwareView::Phase;
  draw_dialog(r, d, "FIRMWARE UPDATE");
  const int x = d.body.x + 2 * m.pad;
  const int w = d.body.w - 4 * m.pad;
  const int right = x + w;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const Body body{r, d.body.y, pitch};
  const auto data_line = [&](int row, const char* text) { body.put(row, x, text, Face::Data, Color::ChromeText); };
  const auto label_line = [&](int row, const char* text, Color ink = Color::ChromeDim) { body.put(row, x, text, Face::Label, ink); };
  const auto bar = [&](int row) {
    ui::bar(r, {x, d.body.y + row * pitch + m.gap, w, pitch - 2 * m.gap}, permille_of(v.done, v.total), 1000u, Color::ChromeDim, Color::ChromeText);
  };

  Text line;
  switch (v.phase) {
    case Phase::List: {
      if (!v.msg.empty()) {  // (one line: kMsgChars is what the dialog is wide enough for)
        label_line(0, v.msg.c_str(), Color::ChromeText);
      } else {
        label_line(0, "Choose a file of the Firmware folder.");
      }
      if (v.n == 0) {
        data_line(kLine, "NO .PTF IN FIRMWARE/");
      }
      const int cursor = v.cursor < v.n ? v.cursor : 0;
      const int first = cursor < kRows ? 0 : cursor - kRows + 1;
      for (int k = 0; k < kRows && first + k < static_cast<int>(v.n); ++k) {
        const hal::firmware::File& f = v.files[first + k];
        const int row = kLine + k;
        if (first + k == cursor) {
          data_line(row, ">");
        }
        const int name_x = x + 2 * cell;
        int name_end = right;  // where the next text of the row begins: the size, or on the first file the tag before it
        line.clear();
        append_kib(line, f.size);
        const int size_w = r.text_width(line.c_str(), Face::Label);
        name_end -= size_w;
        body.put(row, right - size_w, line.c_str(), Face::Label, Color::ChromeDim);
        if (f.reset_target) {  // the reset button flashes the first .ptf of the folder
          const char* tag = "RESET TARGET";
          name_end -= 2 * cell + r.text_width(tag, Face::Label);
          body.put(row, name_end, tag, Face::Label, Color::ChromeDim);
        }
        // The name has the room up to `name_end`, one cell free: a name of the full 52 characters is cut to "..." on the first file, whose tag takes the room (the next step shows the whole name).
        const int room_cells = (name_end - name_x) / cell - 1;
        const std::size_t room = room_cells > 0 ? static_cast<std::size_t>(room_cells) : 0u;
        std::size_t len = 0;
        while (len < sizeof f.name && f.name[len] != '\0') {
          ++len;
        }
        if (len <= room) {
          body.put(row, name_x, f.name, Face::Data, Color::ChromeText);
        } else {
          line.clear();
          append_clipped(line, f.name, room > 3 ? room - 3 : 0);
          line.append("...");
          body.put(row, name_x, line.c_str(), Face::Data, Color::ChromeText);
        }
      }
      break;
    }
    case Phase::AskUpdate:
      line.assign("UPDATE TO ");
      line.append(v.chosen());
      line.append("?");
      data_line(0, line.c_str());
      label_line(1, "It is checked first. Nothing is written.");
      break;
    case Phase::Checking:
      line.assign("CHECKING ");
      line.append(v.chosen());
      data_line(0, line.c_str());
      bar(kLine);
      label_line(kLine + 1, "Reading the card.");
      break;
    case Phase::AskInstall: {
      line.assign("IMAGE OK, ");
      append_uint(line, v.image);
      line.append(" B, CRC ");
      char crc[9];  // the CRC-32 of the whole file, as `fw arm` prints it: 8 upper-case digits
      *put_hex2(put_hex2(put_hex2(put_hex2(crc, v.crc >> 24), v.crc >> 16), v.crc >> 8), v.crc) = '\0';
      line.append(crc);
      data_line(0, line.c_str());
      label_line(1, v.chosen(), Color::ChromeText);
      label_line(kLine, "Good for 30 s. Any other key cancels.");
      break;
    }
    case Phase::Installing: {
      const char* warn = "UPDATING: DO NOT POWER OFF";
      const int box_h = pitch - 2 * m.gap;
      r.panel({x, d.body.y + m.gap, r.text_width(warn, Face::Data) + 4 * m.pad, box_h}, Color::ChromeText);  // an inverted chip: the one thing on the screen that matters now
      body.put(0, x + 2 * m.pad, warn, Face::Data, Color::Chrome);
      label_line(1, "The unit restarts by itself.");
      bar(kLine);
      break;
    }
    case Phase::Closed: break;
  }
  const int foot = d.body.y + (kLine + kRows) * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);
  if (v.phase == Phase::List && v.entries > FirmwareView::kMax) {  // a folder that is paged through: where the cursor is ("FILE 14 / 19")
    Text where;
    where.assign("FILE ");
    append_uint(where, v.position() + 1u);
    where.append(" / ");
    append_uint(where, v.entries);
    body.put(kLine + kRows, right - r.text_width(where.c_str(), Face::Label), where.c_str(), Face::Label, Color::ChromeDim);
  }
}

constexpr FirmwareOverlay kFirmwareOverlay{};

}  // namespace o3os::app
