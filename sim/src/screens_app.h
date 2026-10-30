// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The gallery / budget states of OS-002b that draw through the app's screen registry and overlay slot (sim/screens.cpp
// lists them in the registry beside the others): the Song screen, and the overlays over the Pattern and Song screens.
// Private to sim/src.

#include <o3os/app/model.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/voices.h>
#include <o3os/model/project.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/plot/peak_cache.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace o3os::sim {

// The Song screen: typical (its first page), arrangement (the second), worst (a sweep), over a document with the track map (OS-SONG1), and (OS-SONGMAP) with the cursor in the map, its dearest case and the question up.
extern const ScreenState kSongStates[16];
// The overlays: Learn, the Navigator and a notification over the Pattern screen, a notification over the Song screen.
// Each has a typical state and a `_worst` sweep over the dearest content under it. Since OS-009 P1a.1 also the three choosers of the Pattern screen; since OS-SONGMAP the Song screen's TOOLS dialog.
extern const ScreenState kOverlayStates[14];

// The generated parameter page (Instrument PARAMS, OS-002d): the four pages typical, the cutoff envelope, Learn over the filter page, no project,
// and `worst`, the sweep of every page, row, content, controller tag and overlay (screens_params.cpp).
extern const ScreenState kParamsStates[9];

// The SD modules' pages (OS-012b, screens_modules.cpp): a module instrument's page and its second page, the worst page the budgets allow a module, the engine picker,
// MODULES (with apps, and with no card), an app's page running and stopped.
extern const ScreenState kModulesStates[9];

// The hidden INPUT TEST screen (OS-007d): typical (a few controls down, some counted, a turn of the dial) and worst (every control counted
// to 99 and none down, the widest numbers).
extern const ScreenState kInputTestStates[2];

// The File screen (OS-010a, screens_file.cpp): a card with projects (typical), a scan under way, the full list of long names (worst), a card with no
// Projects folder, no card.
extern const ScreenState kFileStates[15];  // (+ OS-WAVEXP slice B: the stems page; OS-FILES part B: the RECENT group)
extern const ScreenState kExportStates[3];  // OS-WAVEXP: the export progress overlay (screens_file.cpp)

// The sample screens' world (screens_sample.cpp, OS-011a): a project with a sample instrument in slot 4 (instrument 5, "BREAK_A": a three second break of
// eight hits, beat slice mode, sixteen even slices, a loop), its PCM in a bank, and the peak cache of it, built once and shared by the Sample load, Wave /
// Chop and Sample edit states. `model()` is a Model that reads them (the files, the config and the history are the state's own).
struct SampleWorld {
  static constexpr unsigned kSlot = 4;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peak_store;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  ui::plot::PeakCache cache;
  std::uint32_t frames = 0;
  SampleWorld();
  app::Model model() const;
};
SampleWorld& sample_world();

// The Sample load screen (screens_sample.cpp): a card of WAV files (typical), the full list of long names (worst), a card without any, no card.
extern const ScreenState kSampleLoadStates[6];

// The Wave / Chop screen (screens_sample.cpp): the WAVE page with the loop and the points (typical), zoomed in on the loop start, the CHOP page with sixteen slices,
// its worst case (48 slices, every number, the pads), an instrument with no sample, and the waveform still being built; and (OS-INSTUI S4) a WAVETABLE instrument, a GRANULAR one and the list of the
// play modes over the WAVE page.
extern const ScreenState kWaveStates[9];

// The Params F1 source page (screens_sample.cpp, OS-INSTUI S3): one shot (the points), a loop, beat slice (the slices), wavetable (the window), granular (the grain), and an instrument with no audio.
extern const ScreenState kSourceStates[6];

// The Sample edit screen (screens_sample.cpp): a selection over the break, zoomed in on its start edge, an instrument with no sample, and the selection with Shift held (the Shift row).
extern const ScreenState kSampleEditStates[4];

// The Config screen (screens_sample.cpp): the MIDI page, DISPLAY, SD with a card and without, INFO, USB, PATTERN, and (OS-FWCONFIG) the six states of the firmware update dialog over INFO: the list,
// question 1, the check, question 2, the hand-over and a failed check.
extern const ScreenState kConfigStates[17];

// The widest text the model's limits allow (screens_legibility.cpp, OS-E2E-5): the Pattern screen with the longest names, tempo 800, instrument 64 and pattern 255, the same with the
// longest strip message, the Instrument page of instrument 64, the File screen of the longest project name and (OS-FWCONFIG) the firmware update list with twelve files of 48 characters and the
// longest line. Read by the legibility sweep (`o3sim --legibility`).
extern const ScreenState kExtremeStates[5];

// The Mixer screen (screens_mixer.cpp, OS-011b): the MIXER page with the cursor on a level, a pan and the master, with no project, and the worst case (sixteen tracks
// with every mark and every meter hot, swept over both fields of every column); the ROUTING, MASTER FX and STEMS pages, each with its typical, edge and worst states.
extern const ScreenState kMixerStates[18];
// The Perform screen (screens_mixer.cpp, OS-011b): slots with FX latched on three tracks and a pad down, the EDIT state, no board, and the worst case (every slot at its widest, EDIT on,
// every track affected and muted, swept over the edit place).
extern const ScreenState kPerformStates[5];

// The Record screen (screens_record.cpp, OS-011c): idle, a take recording, a take under review with its trim window, RESAMPLE on a track, the RADIO page with a chip and without,
// no recorder, and the worst case (a take under review on every page, swept over the rows, every meter hot).
extern const ScreenState kRecordStates[14];

// What the pattern screen's own sweep draws in its worst case (screens.cpp), so the overlays are measured over it.
const app::PatternData& pattern_worst_data(unsigned set);
inline constexpr unsigned kPatternWorstDataSets = 3;  // filled, sparse, and sparse with the dearest note tiles (screens.cpp)

// The Pattern screen over a document (screens_pattern.cpp; OS-009 P1a.1). Part 3 of the `worst` sweep (the marks, the Shift layer and a
// block over the dearest content), the typical Edit state, the block in the worst case and the Shift layer.
inline constexpr unsigned kPatternDocWorstVariants = kPatternWorstDataSets * 2 * 3 * 6;
inline constexpr unsigned kPatternSelectionVariants = kPatternWorstDataSets * 9 * 2 * 2 * 5;
void draw_pattern_doc_worst(ui::Renderer& r, unsigned variant);
void draw_pattern_edit_on(ui::Renderer& r, unsigned variant);
void draw_pattern_selection_worst(ui::Renderer& r, unsigned variant);
void draw_pattern_shift_layer(ui::Renderer& r, unsigned variant);
void draw_pattern_rec(ui::Renderer& r, unsigned variant);
// OS-ZOOM: the zoom levels 8 and 12 over the dearest content (36 variants: three contents, the marks, a block, three cursor tracks).
inline constexpr unsigned kPatternZoomVariants = kPatternWorstDataSets * 2 * 2 * 3;
void draw_pattern_zoom8(ui::Renderer& r, unsigned variant);
void draw_pattern_zoom12(ui::Renderer& r, unsigned variant);
// The document model of a content (0 the demo's pattern, 1..kPatternWorstDataSets the worst-case data sets) with every header mark set or none.
const app::Model& pattern_doc_model(unsigned which, bool marks);
// A Pattern ViewState in Edit with the Shift layer on, at a row and a number of column moves right, with the window's block when `block`.
app::ViewState pattern_doc_worst_view(int row, int track_moves, bool block);

}  // namespace o3os::sim
