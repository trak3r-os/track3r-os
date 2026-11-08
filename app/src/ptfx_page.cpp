// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/ptfx_page.h>

#include <o3os/app/text_format.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace o3os::app {

namespace {

constexpr int kMaxClicks = 1 << 16;
constexpr float kShown = 1.0e7f;  // beyond this a value is shown at the limit (a plugin may declare any float range)

// OS-CEPTFX S4a (docs/specs/OS-CEPTFX.md 3.7; the entry has no version number, so VERSION says no n). By PtfxStatus; the Ok slot is the quarantine.
constexpr const char* kReasons[] = {"PLUGIN QUARANTINED", "NOT A PTFX PLUGIN", "PTFX VERSION NOT SUPPORTED", "NOT FOR THIS TRACKER", "NEEDS NEWER OS", "PLUGIN FILE DAMAGED", "PLUGIN TOO BIG"};
constexpr unsigned kReasonCount = sizeof kReasons / sizeof kReasons[0];

}  // namespace

const char* ptfx_refusal_text(const module::PtfxEntry& e) noexcept {
  const unsigned s = static_cast<unsigned>(e.status);
  return kReasons[s < kReasonCount ? s : kReasonCount - 1u];
}

float PtfxPage::fix(unsigned p, float v) const noexcept {
  const module::PtfxParam& q = info_.params[p];
  if (!std::isfinite(v)) {
    v = 0.0f;
  }
  float lo = q.min;
  float hi = q.max;
  if (kind_[p] == 'l') {
    lo = 0.0f;
    hi = static_cast<float>(nitems_[p] - 1u);
  } else if (!q.editable || !(lo <= hi)) {
    return v;  // a range the plugin made unusable: shown as it is, never edited
  }
  v = v < lo ? lo : (v > hi ? hi : v);
  if (kind_[p] != 'f') {
    const float c = v < -2.0e9f ? -2.0e9f : (v > 2.0e9f ? 2.0e9f : v);
    v = static_cast<float>(static_cast<std::int32_t>(c + (c < 0.0f ? -0.5f : 0.5f)));  // whole, without libm
    v = v < lo ? v + 1.0f : (v > hi ? v - 1.0f : v);  // (a range with a fraction at its ends: the whole value inside it)
  }
  return v;
}

bool PtfxPage::read_list(hal::storage::Storage& storage, hal::storage::FileHandle file, unsigned p) noexcept {
  module::PtfxParam q = info_.params[p];
  nitems_[p] = 0;
  if (q.list_offset >= info_.code_size) {
    return true;
  }
  const std::uint32_t at = q.list_offset;
  const std::size_t want = std::min<std::size_t>(sizeof window_, info_.code_size - at);
  std::size_t got = 0;
  if (storage.seek(file, info_.code_offset + at) != hal::storage::Status::Ok || storage.read(file, window_, want, got) != hal::storage::Status::Ok) {
    return false;
  }
  q.list_offset = 0;  // the window starts at the first string
  unsigned n = 0;
  for (; n < kItems; ++n) {
    if (!module::ptfx_list_item(window_, got, q, n, items_[p][n])) {
      break;
    }
  }
  nitems_[p] = static_cast<std::uint8_t>(n);
  return true;
}

PtfxPage::Load PtfxPage::load(hal::storage::Storage& storage, unsigned index) noexcept {
  forget();
  if (cat == nullptr || index >= cat->count()) {
    return Load::Refused;
  }
  loaded_ = static_cast<std::uint16_t>(index);  // (set from here on: a failed read is not tried again until the listing or the selection changes)
  const module::PtfxEntry& e = cat->entry(index);
  if (e.status != module::PtfxStatus::Ok || e.quarantined) {
    return Load::Refused;
  }
  char path[module::kPtfxFileNameMax + 9];
  std::memcpy(path, "Effects/", 8);
  std::memcpy(path + 8, e.file_name, sizeof e.file_name);
  hal::storage::FileHandle file;
  std::uint8_t header[module::kPtfxHeaderSize];
  std::uint32_t bytes = 0;
  std::size_t got = 0;
  hal::storage::Status s = storage.open(path, file);
  if (s == hal::storage::Status::Ok) {
    s = storage.size(file, bytes);
  }
  if (s == hal::storage::Status::Ok) {
    s = storage.read(file, header, sizeof header, got);
  }
  if (s != hal::storage::Status::Ok) {
    storage.close(file);
    return Load::CardError;
  }
  if (module::ptfx_parse_header(header, got, bytes, info_) != module::PtfxStatus::Ok) {
    storage.close(file);
    return Load::CardError;  // the file is not what the scan saw
  }
  bool ok = true;
  for (unsigned p = 0; p < info_.num_params; ++p) {
    const char t = info_.params[p].type;
    kind_[p] = t == 'l' ? 'l' : (t == 'd' ? 'd' : 'f');
    nitems_[p] = 0;
    if (t == 'l') {
      ok = read_list(storage, file, p) && ok;
      if (nitems_[p] == 0u) {
        kind_[p] = 'd';  // 3.1: a list without a usable string is an integer
      }
    }
  }
  storage.close(file);
  if (!ok) {
    return Load::CardError;
  }
  // A15: the plugin's values move to the front of the store, or start at the defaults; a stored value is held to the range the file has now.
  unsigned at = kSaved - 1u;
  for (unsigned i = 0; i < kSaved; ++i) {
    if (saved_[i].id == e.id) {
      at = i;
      break;
    }
  }
  const Saved keep = saved_[at];
  for (unsigned i = at; i > 0; --i) {
    saved_[i] = saved_[i - 1u];
  }
  saved_[0] = keep;
  ready_ = true;
  if (keep.id != e.id) {
    saved_[0].id = e.id;
    (void)reset_all();
  } else {
    for (unsigned p = 0; p < kParams; ++p) {
      saved_[0].v[p] = p < info_.num_params ? fix(p, saved_[0].v[p]) : 0.0f;
    }
  }
  return Load::Ready;
}

bool PtfxPage::reset(unsigned p) noexcept {
  if (!ready_ || p >= info_.num_params) {
    return false;
  }
  const float d = fix(p, info_.params[p].def);
  const bool changed = d != saved_[0].v[p];
  saved_[0].v[p] = d;
  return changed;
}

bool PtfxPage::reset_all() noexcept {
  bool changed = false;
  for (unsigned p = 0; ready_ && p < info_.num_params; ++p) {
    changed = reset(p) || changed;
  }
  return changed;
}

bool PtfxPage::bump(unsigned p, int clicks, bool coarse) noexcept {
  if (!ready_ || p >= info_.num_params || !info_.params[p].editable) {
    return false;
  }
  const module::PtfxParam& q = info_.params[p];
  clicks = clicks > kMaxClicks ? kMaxClicks : (clicks < -kMaxClicks ? -kMaxClicks : clicks);
  float step = 1.0f;
  if (kind_[p] == 'f') {
    step = (std::isfinite(q.step) && q.step > 0.0f) ? q.step : (q.max - q.min) / 100.0f;
  } else if (kind_[p] == 'd') {
    step = std::isfinite(q.step) && q.step >= 1.0f ? static_cast<float>(static_cast<std::int32_t>(q.step < 1.0e9f ? q.step + 0.5f : 1.0e9f)) : 1.0f;
  }
  const float to = fix(p, saved_[0].v[p] + static_cast<float>(clicks) * (coarse && kind_[p] != 'l' ? 10.0f : 1.0f) * step);
  const bool changed = to != saved_[0].v[p];
  saved_[0].v[p] = to;
  return changed;
}

void PtfxPage::format_value(unsigned p, core::FixedString<kValueChars>& out) const noexcept {
  out.clear();
  if (!ready_ || p >= info_.num_params) {
    return;
  }
  float v = saved_[0].v[p];
  if (kind_[p] == 'l') {
    out.append(items_[p][std::min<unsigned>(static_cast<unsigned>(v < 0.0f ? 0.0f : v), nitems_[p] - 1u)]);
    return;
  }
  v = v < -kShown ? -kShown : (v > kShown ? kShown : v);
  const bool neg = v < 0.0f;
  const float a = neg ? -v : v;
  if (kind_[p] == 'd') {
    const std::uint32_t w = static_cast<std::uint32_t>(a + 0.5f);
    out.append(neg && w != 0u ? "-" : "");
    append_uint(out, w);
  } else {
    const std::uint32_t q = static_cast<std::uint32_t>(a * 100.0f + 0.5f);
    out.append(neg && q != 0u ? "-" : "");
    append_uint(out, q / 100u);
    out.append(".");
    append_dec(out, q % 100u, 2);
  }
  const char* unit = info_.params[p].unit;
  if (unit[0] != '\0') {
    out.append(" ");
    out.append(unit);
  }
}

}  // namespace o3os::app
