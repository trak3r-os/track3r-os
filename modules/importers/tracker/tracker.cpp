// SPDX-License-Identifier: MIT
//
// OS-FLASHDIET I2: the MOD / S3M / MED importers as one importer module, tracker.tmod (kind 3, ABI 1.11, the layout tag in header word 44).
//
// The formats_tracker library's sources are compiled here (modules/CMakeLists.txt: formats/mod, formats/med,
// formats/src/volume_level.cpp and the model and core sources they call), so the import's result is the same code's result. The OS lends
// this module, at o3m_importer.begin, the opened file's read window (o3m_file_window, wrapped below as the readers' RandomSource), its own
// model arena and the import options; step drives the reader, read_pcm serves the samples, report copies the import's report out
// (formats/import_module.h, the shapes both sides compile) and close ends it. OS-FLASHDIET I3: the image does not link the importers
// itself any more; this module on the card (modules/importers/tracker.tmod, in the starter zip) is where they run.
#include <o3module.h>

#include "tracker_gen.h"

#include <new>
#include <cstring>

#include <o3os/core/byte_io.h>
#include <o3os/formats/format.h>
#include <o3os/formats/import_module.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>

namespace {

using o3os::formats::Progress;

// The importers never read a byte of the ByteSource of FormatReader::begin (they take ReadParams::random): this is the one they are given.
class NoBytes final : public o3os::core::ByteSource {
 public:
  o3os::core::IoResult read(void*, std::size_t) noexcept override { return {0, o3os::core::IoStatus::Failed}; }
};

// The OS's read window over the opened file, as the readers' random-access source.
class WindowSource final : public o3os::formats::med::RandomSource {
 public:
  explicit WindowSource(const o3m_file_window* w) noexcept : w_(w) {}
  std::uint32_t size() const noexcept override { return w_->size(w_->ctx); }
  bool read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept override { return w_->read(w_->ctx, offset, dst, n) == 0; }

 private:
  const o3m_file_window* w_;
};

struct State {
  o3os::formats::mod::ImportReader mod;  // serves "mod" and "s3m"
  o3os::formats::med::ImportReader med;  // serves "med"
  const o3m_host* host = nullptr;
  const o3m_file_window* file = nullptr;
  WindowSource window{nullptr};
  o3os::model::ModelArena* arena = nullptr;
  NoBytes no_bytes;
  bool med_active = false;
  bool began = false;
};

static_assert(sizeof(State) <= TRACKER_STATE_BYTES, "module.json: state_bytes is too small for State");

State* state_of(void* raw) { return static_cast<State*>(raw); }

int init(void* raw, const o3m_host* host) {
  State* s = new (raw) State();
  s->host = host;
  return 0;
}

void shutdown(void* raw) { state_of(raw)->~State(); }

// ---- the importer table (ABI 1.11, kind 3) ----------------------------------------------------------------------------------

// The formats this module serves, in the table's order: the OS offers a file to them in turn and names the one it probed by its extension.
static const o3m_format_desc kFormats[3] = {{"mod"}, {"s3m"}, {"med"}};
constexpr std::uint32_t kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

int imp_begin(void* raw, const o3m_file_window* file, std::uint32_t format, void* arena, const void* options, std::uint32_t options_size) {
  State* s = state_of(raw);
  if (file == nullptr || file->read == nullptr || file->size == nullptr || arena == nullptr || options == nullptr ||
      options_size < sizeof(o3os::formats::ImportModuleOptions) || format >= kFormatCount) {
    return -1;
  }
  const bool is_med = std::strcmp(kFormats[format].ext, "med") == 0;
  const auto* opts = static_cast<const o3os::formats::ImportModuleOptions*>(options);
  s->file = file;
  new (&s->window) WindowSource(file);  // RandomSource is not copy-assignable: reconstruct in place
  s->arena = static_cast<o3os::model::ModelArena*>(arena);
  s->med_active = is_med;
  if (is_med) {
    s->med.options() = o3os::formats::med::ImportReader::amiga_default();
    s->med.options().engine_fx = opts->engine != 2u;  // 2 = Modern: the OG / core FX
  } else {
    s->mod.options() = o3os::formats::mod::ImportOptions{};
    s->mod.options().engine_fx = opts->engine == 0u   ? o3os::formats::mod::EngineFx::Auto
                                 : (opts->engine == 1u ? o3os::formats::mod::EngineFx::On : o3os::formats::mod::EngineFx::Off);
  }
  o3os::formats::ReadParams params;
  params.random = &s->window;
  params.size_hint = file->size(file->ctx);
  if (is_med) {
    s->med.begin(s->no_bytes, *s->arena, params);
  } else {
    s->mod.begin(s->no_bytes, *s->arena, params);
  }
  s->began = true;
  return 0;
}

std::uint32_t imp_step(void* raw, std::uint32_t budget) {
  State* s = state_of(raw);
  if (!s->began) {
    return O3M_IMPORT_FAILED;
  }
  const Progress p = s->med_active ? s->med.step(budget) : s->mod.step(budget);
  return p == Progress::Done ? O3M_IMPORT_DONE : (p == Progress::Failed ? O3M_IMPORT_FAILED : O3M_IMPORT_RUNNING);
}

std::int32_t imp_read_pcm(void* raw, std::uint32_t slot, std::uint32_t first, std::uint32_t frames, std::int16_t* dst) {
  State* s = state_of(raw);
  const auto st = s->med_active ? s->med.importer().read_pcm(slot, first, frames, dst) : s->mod.importer().read_pcm(slot, first, frames, dst);
  return st.ok() ? 0 : -1;
}

std::uint32_t imp_report(void* raw, void* out, std::uint32_t size) {
  State* s = state_of(raw);
  o3os::formats::ImportModuleReport r{};
  if (s->med_active) {
    const o3os::formats::med::ImportReport& rep = s->med.importer().report();
    r.common = s->med.report();
    r.is_med = true;
    r.overflow = rep.overflow;
    r.engine_fx = s->med.options().engine_fx;
    r.channels = static_cast<std::uint8_t>(rep.tracks_used);
    r.patterns = rep.patterns;
    r.order = rep.order;
    r.tracks_kept = rep.tracks_kept;
    r.tracks_dropped = rep.tracks_dropped;
    r.instruments = rep.instruments;
    r.instruments_refused = rep.instruments_refused;
    for (unsigned i = 0; i < 256u; ++i) {
      r.cmd_approx += rep.cmd_approx[i];
      r.cmd_dropped += rep.cmd_dropped[i];
    }
    const std::uint32_t rate = s->med.importer().pcm_rate();
    for (unsigned slot = 0; slot < o3os::formats::med::caps::kMaxSamples; ++slot) {
      r.slots[slot].result = static_cast<std::uint8_t>(rep.instrument[slot].result);
      r.slots[slot].frames = rep.instrument[slot].frames;
      r.slots[slot].rate = rate;
    }
  } else {
    const o3os::formats::mod::ImportReport& rep = s->mod.importer().report();
    r.common = s->mod.report();
    r.is_med = false;
    r.overflow = rep.overflow;
    r.engine_fx = rep.engine_fx;
    r.mod_kind = static_cast<std::uint8_t>(rep.kind);
    r.channels = rep.channels;
    r.patterns = rep.med.patterns;
    r.order = rep.med.order;
    r.tracks_kept = rep.med.tracks_kept;
    r.tracks_dropped = rep.med.tracks_dropped;
    r.instruments = rep.med.instruments;
    r.instruments_refused = static_cast<std::uint16_t>(rep.med.instruments_refused + (rep.instruments_beyond > 0xFFFFu ? 0xFFFFu : rep.instruments_beyond));
    for (const o3os::formats::mod::FxTally& f : rep.fx) {
      r.cmd_approx += f.approx;
      r.cmd_dropped += f.dropped;
    }
    for (unsigned slot = 0; slot < o3os::formats::med::caps::kMaxSamples && slot < o3os::formats::mod::caps::kSlots; ++slot) {
      r.slots[slot].result = static_cast<std::uint8_t>(rep.med.instrument[slot].result);
      r.slots[slot].frames = rep.med.instrument[slot].frames;
      r.slots[slot].rate = s->mod.importer().pcm_rate(slot);
    }
  }
  const std::uint32_t n = size < sizeof r ? size : static_cast<std::uint32_t>(sizeof r);
  std::memcpy(out, &r, n);
  return n;
}

void imp_close(void* raw) {
  State* s = state_of(raw);
  s->began = false;  // the readers are single-use: the next begin() starts them again
  s->file = nullptr;
  s->arena = nullptr;
}

static const o3m_importer kImporterTable = {sizeof(o3m_importer), kFormats, kFormatCount, imp_begin, imp_step, imp_read_pcm, imp_report, imp_close};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    TRACKER_ENTRY_HEAD, sizeof(State), TRACKER_ENTRY_PARAMS, init, nullptr, nullptr, nullptr, nullptr, shutdown, TRACKER_ENTRY_VIEWS(nullptr),
    TRACKER_ENTRY_IMP(&kImporterTable),
};
