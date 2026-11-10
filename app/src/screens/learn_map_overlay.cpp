// SPDX-License-Identifier: GPL-3.0-or-later
// The LEARN MAP overlay (OS-LEARNMAP, docs/specs/OS-LEARNMAP.md): every Learn binding plus the encoder's target as one table over the whole screen below the status strip,
// the ENC row first. Columns SOURCE (CC), CH, PARAMETER, INS, MODE, the live VALUE and an activity dot. The window is stateless: the cursor's row stays in view
// (top = cursor - 7, clamped), so no scroll state is kept. The edits are the actions of learn_map_actions.cpp.
#include <o3os/app/frame.h>
#include <o3os/app/learn_list.h>
#include <o3os/app/model.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/app/view_state.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/learn.h>
#include <o3os/ui/components.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

// Column x offsets and the widths of a column's box, in Data-face cells from the row's left edge (LearnMapView column 0..4: SOURCE CH PARAMETER INS MODE).
constexpr int kColX[LearnMapView::kColumns] = {0, 7, 11, 24, 32};
constexpr int kColW[LearnMapView::kColumns] = {6, 3, 12, 7, 8};
constexpr int kColValue = 41;
constexpr int kColDot = 57;

constexpr const char* kModeNames[core::kLearnModeCount] = {"PICKUP", "JUMP", "RELATIVE"};

}  // namespace

Rect LearnMapOverlay::rect(const ui::Theme& theme) const noexcept {
  const ui::Metrics& m = theme.metrics;
  return Rect{0, static_cast<int>(m.strip_h), ui::kScreenW, static_cast<int>(m.header_h) + kFrameRows * m.row_pitch};
}

int LearnMapOverlay::count(const Model& model, const ViewState&) const noexcept { return 1 + static_cast<int>(learn_entry_count(model)); }
int LearnMapOverlay::position(const ViewState& view) const noexcept { return view.learn_map.cursor; }

bool LearnMapOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int n = count(model, view);
  const int to = position < 0 ? 0 : (position >= n ? n - 1 : position);
  const bool moved = to != view.learn_map.cursor || view.learn.confirm || view.learn_map.wait != 0;
  view.learn_map.cursor = static_cast<std::uint8_t>(to);
  view.learn.confirm = false;  // a move disarms CLEAR ALL and a pending NEW / RELEARN
  view.learn_map.wait = 0;
  return moved;
}

bool LearnMapOverlay::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (dx == 0) {
    return dy != 0 && set_position(model, view, position(view) + dy);
  }
  const int to = view.learn_map.column + dx;
  const int col = to < 0 ? 0 : (to >= LearnMapView::kColumns ? LearnMapView::kColumns - 1 : to);
  const bool moved = col != view.learn_map.column || view.learn.confirm || view.learn_map.wait != 0;
  view.learn_map.column = static_cast<std::uint8_t>(col);
  view.learn.confirm = false;
  view.learn_map.wait = 0;
  return moved;
}

void LearnMapOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Rect cover = rect(t);
  r.panel(cover, Color::Pane);
  const int pitch = m.row_pitch;
  const int x0 = 2 * m.pad;
  const int cell = t.geometry(Face::Data).cell_w;
  const int y0 = cover.y;
  const int total = count(model, view);
  const int cursor = view.learn_map.cursor < total ? view.learn_map.cursor : total - 1;
  int top = cursor - kLearnMapRows / 2;
  if (top > total - kLearnMapRows) {
    top = total - kLearnMapRows;
  }
  if (top < 0) {
    top = 0;
  }

  // Row 0: the title (or what the dialog waits for) and the position.
  {
    const int y = ui::caps_y(t, Face::Data, y0, pitch);
    const char* head = "LEARN MAP";
    if (view.learn_map.wait == LearnMapView::kWaitNew) {
      head = "LEARN MAP  MOVE A KNOB: THE NEW BINDING";
    } else if (view.learn_map.wait == LearnMapView::kWaitRelearn) {
      head = "LEARN MAP  MOVE A KNOB: THE NEW SOURCE";
    } else if (view.learn.confirm) {
      head = "LEARN MAP  PRESS AGAIN TO CLEAR EVERY BINDING";
    }
    r.text(x0, y, head, view.learn_map.wait != 0 || view.learn.confirm ? Color::Accent : Color::Text, Face::Data);
    core::FixedString<12> pos;
    (void)append_uint(pos, static_cast<unsigned>(cursor) + 1u);
    pos.append("/");
    (void)append_uint(pos, static_cast<unsigned>(total));
    r.text(cover.x + cover.w - 2 * m.pad - r.text_width(pos.c_str(), Face::Data), y, pos.c_str(), Color::Text, Face::Data);
  }

  // Row 1: the captions.
  {
    const int y = ui::caps_y(t, Face::Data, y0 + pitch, pitch);
    const Color dim = Color::TextDim;
    r.text(x0 + kColX[0] * cell, y, "SOURCE", dim, Face::Data);
    r.text(x0 + kColX[1] * cell, y, "CH", dim, Face::Data);
    r.text(x0 + kColX[2] * cell, y, "PARAMETER", dim, Face::Data);
    r.text(x0 + kColX[3] * cell, y, "INS", dim, Face::Data);
    r.text(x0 + kColX[4] * cell, y, "MODE", dim, Face::Data);
    r.text(x0 + kColValue * cell, y, "VALUE", dim, Face::Data);
  }

  for (int i = 0; i < kLearnMapRows && top + i < total; ++i) {
    const int index = top + i;
    const int ry = y0 + (2 + i) * pitch;
    const bool sel = index == cursor;
    core::LearnBinding b{view.enc_target, 0, 0, static_cast<std::uint8_t>(core::LearnMode::Relative), 0};
    unsigned store = 0;
    unsigned slot = core::kLearnSlots;
    if (index != 0) {
      LearnEntry e{};
      if (!learn_entry_at(model, static_cast<unsigned>(index) - 1u, e)) {
        break;
      }
      b = *e.binding;
      store = static_cast<unsigned>(e.scope);  // LearnScope::Project 0, Device 1: the order of ViewState::learn_map.hit
      slot = e.slot;
    }
    const bool enc = index == 0;
    const Color ink = sel ? Color::SelText : Color::Text;
    if (sel) {
      r.panel(Rect{cover.x, ry, cover.w, pitch}, Color::Sel);
      const int c = view.learn_map.column;
      r.panel(Rect{x0 + kColX[c] * cell - m.pad / 2, ry + m.border, kColW[c] * cell + m.pad, pitch - 2 * m.border}, Color::Accent);
    }
    const auto cx = [&](int col) { return x0 + col * cell; };
    const auto field_ink = [&](int c) { return sel && c == view.learn_map.column ? Color::AccentText : ink; };
    const int y = ui::caps_y(t, Face::Data, ry, pitch);
    const bool bound = !enc || b.param != core::ParamId::None;

    core::FixedString<12> tmp;
    if (enc) {
      tmp.assign("ENC");
    } else {
      tmp.assign("CC ");
      (void)append_dec(tmp, b.cc, 3);
    }
    r.text(cx(kColX[0]), y, tmp.c_str(), field_ink(0), Face::Data);
    tmp.clear();
    if (enc) {
      tmp.assign("--");
    } else if (b.channel == 0) {
      tmp.assign("ALL");
    } else {
      (void)append_dec(tmp, b.channel, 2);
    }
    r.text(cx(kColX[1]), y, tmp.c_str(), field_ink(1), Face::Data);

    ParamScratch scratch;
    const core::ParamDesc* d = bound ? describe_param(model, b.param, scratch) : nullptr;
    r.text(cx(kColX[2]), y, d != nullptr ? d->name : (bound ? "?" : "--"), field_ink(2), Face::Data);
    core::FixedString<48> owner;
    if (bound) {
      owner_text(owner, b);
    } else {
      owner.assign("--");
    }
    r.text(cx(kColX[3]), y, owner.c_str(), field_ink(3), Face::Data);
    r.text(cx(kColX[4]), y, kModeNames[b.mode % core::kLearnModeCount], field_ink(4), Face::Data);

    char value[core::kParamTextChars + 1];
    if (bound) {
      learn_value_text(model, view, b, value);
    } else {
      value[0] = '-';
      value[1] = '-';
      value[2] = '\0';
    }
    r.text(cx(kColValue), y, value, ink, Face::Data);

    if (!enc && slot < core::kLearnSlots && (view.learn_map.hit[store & 1u] & (std::uint32_t{1} << slot)) != 0u) {
      const int dot = pitch / 3;
      r.panel(Rect{cx(kColDot), ry + (pitch - dot) / 2, dot, dot}, Color::Accent);
    }
  }
}

constexpr LearnMapOverlay kLearnMapOverlay{};

}  // namespace o3os::app
