// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/gallery.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/png.h>
#include <o3os/platform/host/sha256.h>
#include <o3os/sim/budget.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/font.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace o3os::sim {

namespace {

constexpr int kScale = EveRaster::kWidth / kThumbW;  // 4: a thumbnail pixel is the mean of 4 x 4
static_assert(kScale * kThumbW == EveRaster::kWidth && kScale * kThumbH == EveRaster::kHeight,
              "the thumbnail is an exact quarter of the screen");

constexpr int kMargin = 12;   // between cells and round the sheet
constexpr int kFrame = 3;     // the verdict frame
constexpr int kLabelH = 20;   // the label under a thumbnail: one 16-px glyph row and padding
constexpr int kCellW = kThumbW + 2 * kFrame;
constexpr int kCellH = kThumbH + 2 * kFrame + kLabelH;

struct Rgb {
  std::uint8_t r, g, b;
};
constexpr Rgb kSheetBg = {36, 38, 46};
constexpr Rgb kLabel = {226, 230, 240};
constexpr Rgb kUnchecked = {96, 100, 116};
constexpr Rgb kOk = {64, 176, 96};
constexpr Rgb kChanged = {224, 72, 64};
constexpr Rgb kNew = {232, 176, 48};

Rgb verdict_colour(GalleryShot::Verdict v) {
  switch (v) {
    case GalleryShot::Verdict::Ok:
      return kOk;
    case GalleryShot::Verdict::Changed:
      return kChanged;
    case GalleryShot::Verdict::New:
      return kNew;
    case GalleryShot::Verdict::Unchecked:
      break;
  }
  return kUnchecked;
}

// A sheet of opaque RGBA pixels with a few drawing helpers.
class Sheet {
 public:
  Sheet(int w, int h) : w_(w), h_(h), px_(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4) {
    fill(0, 0, w, h, kSheetBg);
  }
  void fill(int x0, int y0, int x1, int y1, Rgb c) {
    for (int y = std::max(y0, 0); y < std::min(y1, h_); ++y) {
      for (int x = std::max(x0, 0); x < std::min(x1, w_); ++x) {
        set(x, y, c);
      }
    }
  }
  void blit_thumb(int x0, int y0, const std::vector<std::uint8_t>& thumb) {
    if (thumb.size() != static_cast<std::size_t>(kThumbW) * kThumbH * 4) {
      return;  // a shot without its thumbnail leaves a blank frame
    }
    for (int y = 0; y < kThumbH; ++y) {
      std::memcpy(&px_[(static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x0)) * 4],
                  &thumb[static_cast<std::size_t>(y) * kThumbW * 4], static_cast<std::size_t>(kThumbW) * 4);
    }
  }
  // Text in the 8 x 16 face of the UI (L1 bitmaps, most significant bit leftmost), clipped to `max_chars`.
  // OS-FLASHDIET A4: the face is stored deflated in flash; `face_px` is its inflated pixels (ui::face_bitmap).
  void text(int x, int y, const std::string& s, Rgb c, std::size_t max_chars, const std::uint8_t* face_px) {
    const ui::FontFace& f = ui::font_face(ui::Font::F8x16);
    for (std::size_t i = 0; i < s.size() && i < max_chars; ++i) {
      unsigned ch = static_cast<std::uint8_t>(s[i]);
      if (ch < f.first_char || ch >= static_cast<unsigned>(f.first_char) + f.glyph_count) {
        ch = '?';
      }
      const std::uint8_t* cell = face_px + static_cast<std::size_t>(ch - f.first_char) * f.stride * f.cell_h;
      for (int row = 0; row < f.cell_h; ++row) {
        for (int col = 0; col < f.cell_w; ++col) {
          if ((cell[row * f.stride + col / 8] >> (7 - col % 8)) & 1u) {
            set(x + static_cast<int>(i) * f.cell_w + col, y + row, c);
          }
        }
      }
    }
  }
  const std::uint8_t* data() const { return px_.data(); }
  int width() const { return w_; }
  int height() const { return h_; }

 private:
  void set(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) {
      return;
    }
    std::uint8_t* p = &px_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = 255;
  }
  int w_, h_;
  std::vector<std::uint8_t> px_;
};

// The mean of each 4 x 4 block, rounded to nearest.
std::vector<std::uint8_t> make_thumb(const std::uint8_t* rgba) {
  std::vector<std::uint8_t> thumb(static_cast<std::size_t>(kThumbW) * kThumbH * 4, 255);
  for (int ty = 0; ty < kThumbH; ++ty) {
    for (int tx = 0; tx < kThumbW; ++tx) {
      for (int c = 0; c < 3; ++c) {
        unsigned sum = 0;
        for (int dy = 0; dy < kScale; ++dy) {
          for (int dx = 0; dx < kScale; ++dx) {
            sum += rgba[(static_cast<std::size_t>(ty * kScale + dy) * EveRaster::kWidth +
                         static_cast<std::size_t>(tx * kScale + dx)) * 4 + static_cast<std::size_t>(c)];
          }
        }
        thumb[(static_cast<std::size_t>(ty) * kThumbW + static_cast<std::size_t>(tx)) * 4 + static_cast<std::size_t>(c)] =
            static_cast<std::uint8_t>((sum + (kScale * kScale) / 2) / (kScale * kScale));
      }
    }
  }
  return thumb;
}

std::string join(const std::string& dir, const std::string& name) {
  return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

}  // namespace

int contact_sheet_width(int columns) { return kMargin + columns * (kCellW + kMargin); }

int contact_sheet_height(std::size_t count, int columns) {
  const int rows = static_cast<int>((count + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
  return kMargin + std::max(rows, 1) * (kCellH + kMargin);
}

namespace {

// Calls visit(screen, state, theme index) for every shot that `filter` selects, in render order. The one place that decides what a
// run draws, so that gallery_keys() (and the shard cover test built on it) cannot disagree with render_gallery().
template <typename Visit>
bool for_each_selected(const GalleryFilter& filter, Visit visit) {
  std::size_t position = 0;  // in the sequence that the theme and the prefix leave: shard n takes the positions n, n + shards, ...
  for (unsigned s = 0; s < screen_count(); ++s) {
    const Screen& screen = screens()[s];
    if (std::strncmp(screen.name, filter.prefix.c_str(), filter.prefix.size()) != 0) {
      continue;
    }
    for (unsigned i = 0; i < screen.state_count; ++i) {
      const ScreenState& state = screen.states[i];
      if (!state.in_gallery) {
        continue;  // measured by the budget harness, not drawn
      }
      for (unsigned t = 0; t < ui::kThemeCount; ++t) {
        if (filter.theme != nullptr && static_cast<unsigned>(*filter.theme) != t) {
          continue;
        }
        if (position++ % filter.shards != filter.shard) {
          continue;
        }
        if (!visit(screen, state, t)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool valid_shard(const GalleryFilter& filter) { return filter.shards != 0 && filter.shard < filter.shards; }

}  // namespace

std::vector<std::string> gallery_keys(const GalleryFilter& filter) {
  std::vector<std::string> keys;
  if (!valid_shard(filter)) {
    return keys;
  }
  for_each_selected(filter, [&](const Screen& screen, const ScreenState& state, unsigned t) {
    keys.push_back(std::string("gallery/") + screen.name + "_" + state.name + "_" + ui::theme(ui::theme_id_at(t)).name);
    return true;
  });
  return keys;
}

bool render_gallery(const std::string& dir, std::vector<GalleryShot>& shots, std::string& error,
                    const GalleryFilter& filter, const GalleryObserver* observer) {
  shots.clear();
  if (!valid_shard(filter)) {
    error = "the shard " + std::to_string(filter.shard) + " of " + std::to_string(filter.shards) + " does not exist";
    return false;
  }
  auto raster = std::make_unique<EveRaster>();  // megabytes of RAM_G and framebuffers: off the stack
  RasterDisplay display(*raster);
  if (load_assets(display) != hal::UploadResult::Ok) {
    error = "the faces, tiles and icons could not be uploaded to RAM_G";
    return false;
  }
  if (observer != nullptr && observer->frame) {
    raster->record_glyphs(observer->record_glyphs_below);
  }
  auto buffer = std::make_unique<eve::DisplayListBuffer>();
  eve::DisplayListBuilder builder(display, buffer->words, eve::kMaxDisplayListWords);

  return for_each_selected(filter, [&](const Screen& screen, const ScreenState& state, unsigned t) {
    const ui::Theme& theme = ui::theme(ui::theme_id_at(t));
    const std::string name = std::string(screen.name) + "_" + state.name + "_" + theme.name;
    // A worst case is shown as the variant of its sweep that needs the most words.
    const unsigned variant = state.worst_case ? measure(screen, state, theme).worst_variant : 0;

    const std::size_t drawn = display.frames();
    const ui::Theme& drawn_with = state.fixed_theme != nullptr ? state.fixed_theme() : theme;
    ui::Renderer r(builder, drawn_with);
    if (!r.begin(ui::Color::Pane)) {
      error = name + ": the display could not begin a frame";
      return false;
    }
    state.draw(r, variant);
    if (!r.end() || display.frames() != drawn + 1) {
      error = name + ": the frame did not fit the display list (" + std::to_string(eve::kMaxDisplayListWords) +
              " words) or was refused" +
              (display.render_failures() != 0 ? ": " + display.first_error() : std::string());
      return false;
    }

    GalleryShot shot;
    shot.key = "gallery/" + name;
    shot.file = name + ".png";
    shot.theme = theme.name;
    shot.hash = platform::host::sha256_hex(raster->rgba(), EveRaster::kFramebufferBytes);
    shot.words = display.last_frame_words();
    shot.variant = variant;
    shot.thumb = make_thumb(raster->rgba());
    if (observer != nullptr && observer->frame) {
      observer->frame(shot, *raster, drawn_with);
    }
    if (!dir.empty() && !platform::host::write_png_rgba(join(dir, shot.file).c_str(), raster->rgba(),
                                                        EveRaster::kWidth, EveRaster::kHeight)) {
      error = "cannot write " + join(dir, shot.file);
      return false;
    }
    shots.push_back(std::move(shot));
    return true;
  });
}

bool write_contact_sheet(const std::string& path, const std::vector<GalleryShot>& shots, int columns) {
  // OS-FLASHDIET A4: the 8x16 face the labels are drawn in is stored deflated; inflate it once (the 4 KiB scratch
  // size, the face is 1,520 B). A stream the build made cannot fail (the converter's self-test round-trips it).
  std::vector<std::uint8_t> face_px(ui::kFaceScratchBytes);
  if (!ui::face_bitmap(ui::Font::F8x16, face_px.data(), face_px.size())) {
    return false;
  }
  Sheet sheet(contact_sheet_width(columns), contact_sheet_height(shots.size(), columns));
  for (std::size_t n = 0; n < shots.size(); ++n) {
    const int col = static_cast<int>(n % static_cast<std::size_t>(columns)), row = static_cast<int>(n / static_cast<std::size_t>(columns));
    const int x0 = kMargin + col * (kCellW + kMargin), y0 = kMargin + row * (kCellH + kMargin);
    sheet.fill(x0, y0, x0 + kCellW, y0 + kThumbH + 2 * kFrame, verdict_colour(shots[n].verdict));
    sheet.blit_thumb(x0 + kFrame, y0 + kFrame, shots[n].thumb);
    const std::string label = shots[n].key.substr(shots[n].key.find('/') + 1) + " " + std::to_string(shots[n].words) + "w";
    sheet.text(x0, y0 + kThumbH + 2 * kFrame + 2, label, kLabel, static_cast<std::size_t>(kCellW / 8), face_px.data());
  }
  return platform::host::write_png_rgba(path.c_str(), sheet.data(), sheet.width(), sheet.height());
}

}  // namespace o3os::sim
