// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The simulator's own project loader (OS-E2E-1b): the app::ProjectSink behind File > OPEN, NEW and NEW > KEEP INSTRUMENTS once the simulator has a card (the scenario command `card`). The device's is
// CardProject in platform/mk66/src/app_main.cpp, the image's composition root, which is firmware and which nothing may link; this one steps the same loaders in the same order and hands the document
// over as the platform's finish_load does:
//
//   Verify    (OS-QAFIX-4, an open of a project folder) app::ProjectVerify: the file is checked (native: framing, versions, counts, CRC; OG: header and size) before anything is replaced. A file that is
//             refused ends the open here: the document, the history, the transport and the voices are untouched, refused() is true and detail() says why
//   Project   app::NativeFolderLoader (<folder>/project.o3pj, or its .bak, or the fresh form of NEW) or app::OgFolderLoader (project.mt, patterns/, instruments/), up to 16 KB a pass
//   Samples   app::OgSampleLoader: the PCM of the sample instruments into the sample memory and the sample bank (none for a fresh project)
//   Import    (OS-E2E-9, File > IMPORT) app::ModuleImport, the MOD / S3M and OctaMED importers behind the IMPORT page, in place of the Project phase; its samples are decoded on request into the sample memory
//             (OgSampleLoader::begin_feed), the document is the module's, on no card (dirty), and the voices are the Amiga engine's where the import said so
//   end       the undo history reset, the app attached to the document again, App::document_replaced, the view on the project's first pattern (the saved actual-pattern field of an OG project file, our choice)
//
// The app's frames are held while it runs, as the platform's pass holds them: Simulator::step() calls pump() instead of the app's pass until busy() ends. With the engine on (`audio on`, E2E-2: attach_sound) a load starts as the
// platform's does, with a Stop on the transport, the live notes let go and the Settle (the voices let to die away, 4 to 250 ms) before the first slice; the voices are then the pool the engine renders. Left out,
// because nothing in the simulator stands behind it: the project's MIDI and USB parts (apply_project_platform_state), the SD modules (a module instrument stays unbound: E2E-7), the demo kit that a project
// with no sample instrument gets, and the log. Without `audio on` the voices are a pool of its own that nothing renders and the load starts at once.
//
// Main loop only (the simulator's thread). No firmware file is touched: everything here uses the app, audio, model and hal libraries as the tests do.

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/module_import.h>
#include <o3os/app/native_load.h>
#include <o3os/app/og_folder.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/storage.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace o3os::sim {

class SimProject final : public app::ProjectSink {
 public:
  static constexpr std::size_t kBytesPerPass = 16 * 1024;  // the platform's kLoadBytesPerPass: about 2 ms of card a pass

  // Everything is the simulator's and outlives this object: the document (`arena` and the `editor` over it), the `app` it hands the document back to, the `card`, the voices and the bank the samples
  // are bound to, and the sample memory (`samples`: `sample_count` int16 values, 4-byte aligned).
  SimProject(model::ModelArena& arena, model::Editor& editor, app::App& app, hal::storage::Storage& card, audio::VoicePool& voices, app::SampleBank& bank, std::int16_t* samples,
             std::uint32_t sample_count);
  ~SimProject();

  // ---- app::ProjectSink
  Start open(const char* folder, Format format) noexcept override;
  Start start_new() noexcept override { return begin_fresh(false); }
  Start start_new_keeping_instruments() noexcept override { return begin_fresh(true); }
  // OS-E2E-9: File > IMPORT (platform/mk66/src/app_main.cpp start_module_import): the module file replaces the document through the same hand-over.
  Start import_module(const char* file, app::ImportEngine engine) noexcept override;
  bool module_file(const char* file) noexcept override;  // OS-IMPORT-ANY I4: the platform's, over the simulator's card
  const app::ModuleSummary* import_summary() const noexcept override { return &module_->summary(); }
  bool busy() const noexcept override { return phase_ != Phase::Idle; }
  const char* folder() const noexcept override;
  bool loaded() const noexcept override;
  const char* detail() const noexcept override;
  bool refused() const noexcept override { return refused_; }

  // OS-E2E-2: with the real engine (`audio on`), a load starts as the platform's does: a Stop on the transport, the live notes let go, and the voices let to die away (the Settle: at least 4 ms and until
  // none is in use, at most 250 ms) before the first slice. Without it nothing sounds and the load starts at once, as before. The sinks must outlive this object.
  void attach_sound(app::TransportSink* transport, app::PreviewSink* preview) noexcept {
    transport_ = transport;
    preview_ = preview;
  }

  // One slice of the load (nothing when none runs): the simulator calls it once a tick in place of the app's pass while busy(). The pass that ends the load hands the document over.
  void pump() noexcept;

 private:
  enum class Phase : std::uint8_t { Idle, Verify, Settle, Project, Samples };

  Start begin_fresh(bool keep_instruments) noexcept;
  Start begin_open() noexcept;  // the verified folder: the loader of its format, then begin_replace()
  void begin_replace() noexcept;
  void begin_samples() noexcept;
  void finish() noexcept;

  model::ModelArena& arena_;
  model::Editor& editor_;
  app::App& app_;
  hal::storage::Storage& card_;
  audio::VoicePool& voices_;
  app::SampleBank& bank_;
  std::int16_t* samples_mem_;
  std::uint32_t sample_count_;
  app::TransportSink* transport_ = nullptr;  // attach_sound()
  app::PreviewSink* preview_ = nullptr;
  std::uint32_t settle_ms_ = 0;
  Phase phase_ = Phase::Idle;
  bool refused_ = false;  // the last open ended in its Verify phase (the document is as it was); refused_why_ is the words
  const char* refused_why_ = "";
  bool native_ = false;  // the last load that was started is the native loader's (a fresh project included), not the OG's
  std::unique_ptr<app::OgFolderLoader> og_;       // (the readers inside are not stack objects)
  std::unique_ptr<app::NativeFolderLoader> native_loader_;
  std::unique_ptr<app::ProjectVerify> verify_;
  std::unique_ptr<app::OgSampleLoader> sample_loader_;
  std::unique_ptr<app::ModuleImport> module_;  // the importers inside are not stack objects either (about 39 KB)
};

}  // namespace o3os::sim
