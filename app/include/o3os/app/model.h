// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/dir_scan.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/song_data.h>
#include <o3os/app/system_load.h>
#include <o3os/hal/storage.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>
#include <o3os/model/settings.h>

namespace o3os::app {

class SampleBank;
class ModuleUi;
class Recorder;

}  // namespace o3os::app

namespace o3os::ui::plot {
class PeakCache;
}

namespace o3os::app {

// What the screens show and the actions change: the project as the UI sees it. Today both parts are static demo data
// (OS-001b's pattern, OS-002b's order list); the real project model (OS-004) replaces them behind the same name. A screen
// reads it (`render(const Model&, ...)`), only actions write it (ADR 0009 mechanism 6).
//
// The parameter screens (OS-002d) read the real thing: `project` is the attached document's project (null without one: the screens then
// show the descriptors' defaults and say there is no project), and `device` is the device's settings block with its Learn store (the
// platform loads it from the settings file; init() it before use, the App does). Both are written only by actions.
//
// The File screen (OS-010a) reads the listing of the folder it browses through `files`: the App's scan of the card (a job that fills it a few
// entries a pass; read-only here, like `project`). nullptr in a bare test: the screen then shows an empty card.
//
// The sample screens (OS-011a: Sample load, Wave / Chop, Sample edit) read the PCM of an instrument slot and its handover state through `samples` (read-only,
// like `project`); only actions write the instrument, and the App's pass the bank.
//
// The Pattern screen (OS-009 D1) shows the attached document's pattern, and the demo `pattern` while there is none: read-only, no edit
// runs on it. `undo_depth` is the document's journal depth (the info line's "undo n"); the App refreshes it before every frame.
// Where the voice of the last live note is in the sample of an instrument (OS-FU3: the playhead of Wave / Chop and Sample edit, and of the Record take), refreshed by the App about 30 times a
// second from the render's published word (audio::VoicePool::live_position). `frame` is in the instrument's sample (the take's, on the Record screen).
struct LiveHead {
  bool on = false;
  std::uint8_t slot = 0;  // the instrument slot it plays (the audition's descriptor reports the slot the held Preview is for)
  std::uint32_t frame = 0;
  bool operator==(const LiveHead& o) const noexcept { return on == o.on && slot == o.slot && frame == o.frame; }
  bool operator!=(const LiveHead& o) const noexcept { return !(*this == o); }
};

struct ModuleSummary;  // module_import.h

struct Model {
  PatternData pattern;
  SongData song;
  const model::Project* project = nullptr;
  const DirScan* files = nullptr;
  const char* project_folder = "";  // the folder the open project was loaded from ("Projects/My Song"), "" for a new or imported one: the File header names the project by it when it has no title (OS-NAMES); owned by the App's sink
  bool project_unsaved = false;  // OS-AUDIT-A6 (E-FILE-17): the open project has changes no save has taken (SaveService::dirty, the state the questions and the Power hold use); told by App::poll_files while the File screen is up; the File header marks it
  const ModuleSummary* import_report = nullptr;  // the File screen's REPORT page: the last module import (OS-FILES); nullptr in a bare test
  const ui::plot::PeakCache* peaks = nullptr;  // the waveform of the sample in view (the App's cache, valid when it is built for that slot's PCM); nullptr in a bare test
  const hal::storage::Storage* card = nullptr;  // the Config screen's SD page reads what the card layer knows (nullptr in a bare test)
  const char* product = "";                     // the Info page: the platform tells the App what it is and which build (App::set_build_info)
  const char* build_id = "";
  bool firmware = false;                        // the platform has an update path (App::attach_firmware): the INFO page labels F8 UPDATE (OS-FWCONFIG)
  const SampleBank* samples = nullptr;  // the sample screens' view of the arena: the PCM of each instrument (OS-011a); nullptr in a bare test
  const MixBoard* mix = nullptr;  // the Mixer screen's runtime side: routing, cue level, meters, the stem map (OS-011b); nullptr in a bare test (the pages then show defaults)
  const PerformBoard* perform = nullptr;  // the Perform screen's slots (OS-011b); nullptr in a bare test (the screen then shows the default slots)
  ModuleUi* modules = nullptr;  // the SD modules' side (OS-012b): the Instrument page's module controls and views, the engine picker, the MODULES screen; nullptr in a bare test
  const Recorder* rec = nullptr;  // the Record screen's recorder (OS-011c); nullptr in a bare test (the screen then says there is no recorder)
  LiveHead head;    // the playhead of the sample screens (OS-FU3)
  SystemLoad load;  // the status strip's CPU and MEM (OS-FU3): the App refreshes it every pass (system_load.h)
  model::Settings device;
  unsigned undo_depth = 0;
  const model::LaneStore* lanes = nullptr;  // OS-MIDIREC-P1 S6: the document's MIDI take lanes (the Editor's store), refreshed with undo_depth; the EVENTS list shows them as lane rows; nullptr: none
};

}  // namespace o3os::app
