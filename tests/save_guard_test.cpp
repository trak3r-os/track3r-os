// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal/save_guard.h (OS-007n): the only paths the save layer may create, rename to or remove. Name validation (letters, digits, space, '_', '-', '.'; no leading or
// trailing junk, no "..", no reserved device name, 31 bytes), every guarded path in every case, the rules of each operation, and the never-delete rules: no Target and no
// folder is removable, create takes a Temp only, rename has three pairs and nothing else. Pure, so it also runs wherever the unit binary does.
#include <doctest/doctest.h>

#include <o3os/hal/save_guard.h>
#include <o3os/hal/storage.h>

#include <string>

namespace sv = o3os::hal::storage::save;
using sv::Place;

TEST_CASE("save guard: names") {
  CHECK(sv::valid_name("Track 01"));
  CHECK(sv::valid_name("a"));
  CHECK(sv::valid_name("My-Song_2.v3"));
  CHECK(sv::valid_name("REC_001"));
  CHECK(sv::valid_name(std::string(sv::kNameMax, 'x').c_str()));
  CHECK_FALSE(sv::valid_name(std::string(sv::kNameMax + 1, 'x').c_str()));
  CHECK_FALSE(sv::valid_name(nullptr));
  CHECK_FALSE(sv::valid_name(""));
  CHECK_FALSE(sv::valid_name(" lead"));
  CHECK_FALSE(sv::valid_name("trail "));
  CHECK_FALSE(sv::valid_name("trail."));
  CHECK_FALSE(sv::valid_name(".hidden"));
  CHECK_FALSE(sv::valid_name("-dash"));
  CHECK_FALSE(sv::valid_name("a..b"));
  CHECK_FALSE(sv::valid_name(".."));
  CHECK_FALSE(sv::valid_name("."));
  CHECK_FALSE(sv::valid_name("a/b"));
  CHECK_FALSE(sv::valid_name("a\\b"));
  CHECK_FALSE(sv::valid_name("a:b"));
  CHECK_FALSE(sv::valid_name("a*b"));
  CHECK_FALSE(sv::valid_name("a?b"));
  CHECK_FALSE(sv::valid_name("a\"b"));
  CHECK_FALSE(sv::valid_name("a<b"));
  CHECK_FALSE(sv::valid_name("a|b"));
  CHECK_FALSE(sv::valid_name("tab\tname"));
  CHECK_FALSE(sv::valid_name("caf\xC3\xA9"));  // UTF-8 is not written by this layer
  for (const char* reserved : {"CON", "con", "Prn", "AUX", "nul", "NUL.txt", "COM1", "com9", "LPT3", "lpt1.wav"}) {
    CHECK_FALSE(sv::valid_name(reserved));
  }
  for (const char* fine : {"CONSOLE", "COM0", "COM10", "LPT", "AUXILIARY", "NULL", "com"}) {
    CHECK(sv::valid_name(fine));
  }
}

TEST_CASE("save guard: every guarded path, in any case") {
  CHECK(sv::classify("Projects") == Place::Folder);
  CHECK(sv::classify("projects") == Place::Folder);
  CHECK(sv::classify("SAMPLES") == Place::Folder);
  CHECK(sv::classify("Settings") == Place::Folder);
  CHECK(sv::classify("Projects/Track 01") == Place::Folder);
  CHECK(sv::classify("PROJECTS/track 01/PROJECT.O3PJ") == Place::Target);
  CHECK(sv::classify("Projects/Track 01/project.o3pj") == Place::Target);
  CHECK(sv::classify("Projects/Track 01/project.o3pj.bak") == Place::Backup);
  CHECK(sv::classify("Projects/Track 01/Project.O3PJ.BAK") == Place::Backup);
  CHECK(sv::classify("Projects/Track 01/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify("Projects/Track 01/_O3SAVE.TMP") == Place::Temp);
  CHECK(sv::classify("Samples/REC_001.wav") == Place::Target);
  CHECK(sv::classify("samples/REC_001.WAV") == Place::Target);
  CHECK(sv::classify("Samples/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify("Settings/device.o3st") == Place::Target);
  CHECK(sv::classify("Settings/DEVICE.O3ST.bak") == Place::Backup);
  CHECK(sv::classify("Settings/_o3save.tmp") == Place::Temp);

  // Everything else is None: the card's root, other folders, other names, deeper paths, the Firmware writer's own place, a leftover test-card marker file.
  CHECK(sv::classify(nullptr) == Place::None);
  for (const char* none : {"", "/", "Projects/", "/Projects", "/Projects/Track 01", "Projects//Track 01", "Projects/Track 01/", ".o3-test-card", "/.o3-test-card",
                           "Firmware", "Firmware/_o3up.tmp", "Firmware/x.ptf", "Workspace", "Workspace/project.mt", "modules/instruments/tb303.tmod", "README.TXT",
                           "Projects/..", "Projects/.", "Projects/../Samples", "Projects/Track 01/..", "Projects/Track 01/../x/project.o3pj",
                           "Projects/Track 01/project.mt", "Projects/Track 01/patterns", "Projects/Track 01/patterns/pattern_01.mtp",
                           "Projects/Track 01/instruments/instrument_01.mti", "Projects/Track 01/project.o3pj.bak.bak", "Projects/Track 01/project.o3pj.tmp",
                           "Projects/Track 01/other.o3pj", "Projects/Track 01/x/project.o3pj", "Projects/a/b/c/d", "Projects/CON/project.o3pj",
                           "Projects/ lead/project.o3pj", "Projects/trail /project.o3pj", "Samples/x.mp3", "Samples/x.wav.bak", "Samples/.wav", "Samples/ .wav",
                           "Samples/sub/x.wav", "Samples/CON.wav", "Samples/x..wav", "Samples/_o3save.tmp.bak", "Settings/other.o3st", "Settings/device.o3st.tmp",
                           "Settings/sub/device.o3st", "Settings/device.o3st.bak.bak", "Projectsx", "xProjects/a", "Sample/x.wav", "Setting/device.o3st"}) {
    INFO("path: " << none);
    CHECK(sv::classify(none) == Place::None);
  }
  CHECK(sv::classify((std::string("Samples/") + std::string(sv::kNameMax, 'x') + ".wav").c_str()) == Place::Target);
  CHECK(sv::classify((std::string("Samples/") + std::string(sv::kNameMax + 1, 'x') + ".wav").c_str()) == Place::None);
  CHECK(sv::classify((std::string("Projects/") + std::string(sv::kNameMax + 1, 'x') + "/project.o3pj").c_str()) == Place::None);
}

TEST_CASE("save guard: what each operation may touch") {
  // make_dir: a folder of the list only.
  CHECK(sv::may_make_dir("Projects"));
  CHECK(sv::may_make_dir("Projects/Track 01"));
  CHECK(sv::may_make_dir("Samples"));
  CHECK(sv::may_make_dir("Settings"));
  CHECK_FALSE(sv::may_make_dir("Firmware"));
  CHECK_FALSE(sv::may_make_dir("Projects/Track 01/patterns"));
  CHECK_FALSE(sv::may_make_dir("Samples/sub"));
  CHECK_FALSE(sv::may_make_dir("Projects/Track 01/project.o3pj"));
  CHECK_FALSE(sv::may_make_dir(nullptr));

  // create: a Temp only: a final name never gets bytes directly.
  CHECK(sv::may_create("Projects/Track 01/_o3save.tmp"));
  CHECK(sv::may_create("Samples/_o3save.tmp"));
  CHECK(sv::may_create("Settings/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("Projects/Track 01/project.o3pj"));
  CHECK_FALSE(sv::may_create("Projects/Track 01/project.o3pj.bak"));
  CHECK_FALSE(sv::may_create("Samples/REC_001.wav"));
  CHECK_FALSE(sv::may_create("Settings/device.o3st"));
  CHECK_FALSE(sv::may_create("Projects/Track 01"));
  CHECK_FALSE(sv::may_create("_o3save.tmp"));

  // remove: a Temp or a Backup: never a Target, never a folder, never anything of the user's.
  CHECK(sv::may_remove("Projects/Track 01/_o3save.tmp"));
  CHECK(sv::may_remove("Samples/_o3save.tmp"));
  CHECK(sv::may_remove("Projects/Track 01/project.o3pj.bak"));
  CHECK(sv::may_remove("Settings/device.o3st.bak"));
  CHECK_FALSE(sv::may_remove("Projects/Track 01/project.o3pj"));
  CHECK_FALSE(sv::may_remove("Settings/device.o3st"));
  CHECK_FALSE(sv::may_remove("Samples/REC_001.wav"));
  CHECK_FALSE(sv::may_remove("Projects"));
  CHECK_FALSE(sv::may_remove("Projects/Track 01"));
  CHECK_FALSE(sv::may_remove("Samples"));
  CHECK_FALSE(sv::may_remove("Projects/Track 01/project.mt"));
  CHECK_FALSE(sv::may_remove("Projects/Track 01/patterns/pattern_01.mtp"));
  CHECK_FALSE(sv::may_remove("Samples/other.wav.bak"));
  CHECK_FALSE(sv::may_remove(".o3-test-card"));
  CHECK_FALSE(sv::may_remove("Firmware/x.ptf"));
  CHECK_FALSE(sv::may_remove(nullptr));

  // rename: Temp -> Target, Target -> its own Backup, Backup -> its own Target, all in one folder.
  CHECK(sv::may_rename("Projects/Track 01/_o3save.tmp", "Projects/Track 01/project.o3pj"));
  CHECK(sv::may_rename("projects/track 01/_O3SAVE.TMP", "PROJECTS/Track 01/Project.o3pj"));
  CHECK(sv::may_rename("Projects/Track 01/project.o3pj", "Projects/Track 01/project.o3pj.bak"));
  CHECK(sv::may_rename("Projects/Track 01/project.o3pj.bak", "Projects/Track 01/project.o3pj"));
  CHECK(sv::may_rename("Samples/_o3save.tmp", "Samples/REC_001.wav"));
  CHECK(sv::may_rename("Settings/_o3save.tmp", "Settings/device.o3st"));
  CHECK(sv::may_rename("Settings/device.o3st", "Settings/device.o3st.bak"));
  CHECK(sv::may_rename("Settings/device.o3st.bak", "Settings/device.o3st"));
  // ... and nothing else:
  CHECK_FALSE(sv::may_rename("Projects/Track 01/project.o3pj", "Projects/Track 02/project.o3pj.bak"));  // another folder
  CHECK_FALSE(sv::may_rename("Projects/Track 01/_o3save.tmp", "Projects/Track 02/project.o3pj"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/_o3save.tmp", "Projects/Track 01/project.o3pj.bak"));  // a temp does not become a backup
  CHECK_FALSE(sv::may_rename("Projects/Track 01/project.o3pj.bak", "Projects/Track 01/_o3save.tmp"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/project.o3pj", "Projects/Track 01/_o3save.tmp"));
  CHECK_FALSE(sv::may_rename("Settings/device.o3st", "Projects/Track 01/project.o3pj.bak"));
  CHECK_FALSE(sv::may_rename("Samples/REC_001.wav", "Samples/REC_002.wav"));  // an existing WAV is never renamed
  CHECK_FALSE(sv::may_rename("Samples/REC_001.wav", "Samples/REC_001.wav.bak"));
  CHECK_FALSE(sv::may_rename("Samples/REC_001.wav", "Samples/_o3save.tmp"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/project.o3pj.bak", "Projects/Track 01/other.o3pj"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/project.mt", "Projects/Track 01/project.o3pj"));  // a user's file never becomes ours
  CHECK_FALSE(sv::may_rename("Projects/Track 01/_o3save.tmp", "Projects/Track 01/project.mt"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01", "Projects/Track 02"));  // a folder is never renamed (OS-FILES part B dropped RENAME: the test case below)
  CHECK_FALSE(sv::may_rename("Projects/Track 01/_o3save.tmp", "Projects/Track 01/_o3save.tmp"));
  CHECK_FALSE(sv::may_rename(nullptr, "Samples/x.wav"));
  CHECK_FALSE(sv::may_rename("Samples/_o3save.tmp", nullptr));
  CHECK_FALSE(sv::may_rename("_o3save.tmp", "x.wav"));
}

TEST_CASE("save guard: join builds paths the guard knows, and refuses what does not fit") {
  char out[sv::kPathCap];
  CHECK(sv::join(out, sizeof out, "Projects", "Track 01", "project.o3pj"));
  CHECK(std::string(out) == "Projects/Track 01/project.o3pj");
  CHECK(sv::join(out, sizeof out, "Samples"));
  CHECK(std::string(out) == "Samples");
  CHECK(sv::join(out, sizeof out, "Samples", nullptr, "x.wav"));
  CHECK(std::string(out) == "Samples/x.wav");
  // The longest guarded path fits the capacity.
  const std::string longest = std::string("Projects/") + std::string(sv::kNameMax, 'x') + "/" + sv::kProjectFile + sv::kBackupSuffix;
  CHECK(longest.size() + 1 <= sv::kPathCap);
  CHECK(sv::join(out, sizeof out, "Projects", std::string(sv::kNameMax, 'x').c_str(), (std::string(sv::kProjectFile) + sv::kBackupSuffix).c_str()));
  CHECK(sv::classify(out) == Place::Backup);
  char tiny[8];
  CHECK_FALSE(sv::join(tiny, sizeof tiny, "Projects", "Track 01"));
  CHECK(tiny[0] == '\0');
  // The guard's paths are paths of hal::storage::valid_path too (what the read side takes).
  CHECK(o3os::hal::storage::valid_path("Projects/Track 01/project.o3pj.bak"));
  CHECK(o3os::hal::storage::valid_path("Samples/REC_001.wav"));
}

TEST_CASE("save guard: a project's samples folder and the files in it (OS-FU2)") {
  CHECK(sv::classify("Projects/Track 01/samples") == Place::Folder);
  CHECK(sv::classify("PROJECTS/track 01/SAMPLES") == Place::Folder);
  CHECK(sv::classify("Projects/Track 01/samples/instr01.wav") == Place::Target);
  CHECK(sv::classify("Projects/Track 01/Samples/INSTR01.WAV") == Place::Target);
  CHECK(sv::classify("Projects/Track 01/samples/instr01.wav.bak") == Place::Backup);
  CHECK(sv::classify("Projects/Track 01/samples/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify((std::string("Projects/") + std::string(sv::kNameMax, 'x') + "/samples/" + std::string(sv::kNameMax, 'y') + ".wav.bak").c_str()) == Place::Backup);
  for (const char* none : {"Projects/Track 01/samples/", "Projects/Track 01/samples/x.mp3", "Projects/Track 01/samples/.wav", "Projects/Track 01/samples/ .wav", "Projects/Track 01/samples/CON.wav",
                           "Projects/Track 01/samples/x..wav", "Projects/Track 01/samples/x.wav.bak.bak", "Projects/Track 01/samples/x.bak", "Projects/Track 01/samples/x.wav.tmp",
                           "Projects/Track 01/samples/sub/x.wav", "Projects/Track 01/samples/_o3save.tmp.bak", "Projects/Track 01/other/x.wav", "Projects/CON/samples/x.wav",
                           "Projects/Track 01/samples/project.o3pj", "Projects/Track 01/samples/a/b/c", "Samples/sub/x.wav", "Settings/samples/x.wav"}) {
    INFO("path: " << none);
    CHECK(sv::classify(none) == Place::None);
  }
  CHECK(sv::classify((std::string("Projects/Track 01/samples/") + std::string(sv::kNameMax + 1, 'x') + ".wav").c_str()) == Place::None);
  // operations: the folder may be made, a Temp created, a Target never created or removed, a Backup removed
  CHECK(sv::may_make_dir("Projects/Track 01/samples"));
  CHECK_FALSE(sv::may_make_dir("Projects/Track 01/samples/sub"));
  CHECK_FALSE(sv::may_make_dir("Projects/Track 01/samples/instr01.wav"));
  CHECK(sv::may_create("Projects/Track 01/samples/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("Projects/Track 01/samples/instr01.wav"));
  CHECK_FALSE(sv::may_create("Projects/Track 01/samples/instr01.wav.bak"));
  CHECK(sv::may_remove("Projects/Track 01/samples/_o3save.tmp"));
  CHECK(sv::may_remove("Projects/Track 01/samples/instr01.wav.bak"));
  CHECK_FALSE(sv::may_remove("Projects/Track 01/samples/instr01.wav"));
  CHECK_FALSE(sv::may_remove("Projects/Track 01/samples"));
  CHECK(sv::may_rename("Projects/Track 01/samples/_o3save.tmp", "Projects/Track 01/samples/instr01.wav"));
  CHECK(sv::may_rename("Projects/Track 01/samples/instr01.wav", "Projects/Track 01/samples/instr01.wav.bak"));
  CHECK(sv::may_rename("Projects/Track 01/samples/instr01.wav.bak", "Projects/Track 01/samples/instr01.wav"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/samples/instr01.wav", "Projects/Track 01/samples/instr02.wav"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/samples/instr01.wav", "Projects/Track 02/samples/instr01.wav.bak"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/samples/_o3save.tmp", "Projects/Track 01/samples/instr01.wav.bak"));
  CHECK_FALSE(sv::may_rename("Projects/Track 01/samples/instr01.wav", "Projects/Track 01/instr01.wav.bak"));  // another folder
  // the longest path fits kPathCap
  const std::string longest = std::string("Projects/") + std::string(sv::kNameMax, 'x') + "/" + sv::kProjectSamplesDir + "/" + std::string(sv::kNameMax, 'y') + ".wav" + sv::kBackupSuffix;
  CHECK(longest.size() + 1 <= sv::kPathCap);
}

TEST_CASE("save guard: the export folder and the WAVs in it (OS-WAVEXP)") {
  CHECK(sv::classify("Export") == Place::Folder);
  CHECK(sv::classify("EXPORT") == Place::Folder);
  CHECK(sv::classify("Export/Track 01") == Place::Folder);
  CHECK(sv::classify("export/track 01/Track 01_Song0001.wav") == Place::Target);
  CHECK(sv::classify("Export/Track 01/Track 01_P7_0001.WAV") == Place::Target);
  CHECK(sv::classify("Export/Track 01/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify("Export/Track 01/_O3SAVE.TMP") == Place::Temp);
  // The longest export path fits the capacity, and so does a stem of the longest name.
  const std::string longest = std::string("Export/") + std::string(sv::kNameMax, 'x') + "/" + std::string(sv::kNameMax, 'y') + ".wav";
  CHECK(longest.size() + 1 <= sv::kPathCap);
  CHECK(sv::classify(longest.c_str()) == Place::Target);
  CHECK(sv::classify((std::string("Export/") + std::string(sv::kNameMax + 1, 'x') + "/a.wav").c_str()) == Place::None);
  CHECK(sv::classify((std::string("Export/x/") + std::string(sv::kNameMax + 1, 'y') + ".wav").c_str()) == Place::None);
  // No backup form, no other file, no deeper folder, no stems yet (a phase 2 folder is a decision of its own).
  for (const char* none : {"Export/", "/Export", "Export//Track 01", "Export/Track 01/", "Export/..", "Export/.", "Export/CON/a.wav", "Export/ lead/a.wav", "Export/trail /a.wav",
                           "Export/Track 01/a.wav.bak", "Export/Track 01/a.mp3", "Export/Track 01/.wav", "Export/Track 01/ .wav", "Export/Track 01/CON.wav", "Export/Track 01/x..wav",
                           "Export/Track 01/a.wav.tmp", "Export/Track 01/_o3save.tmp.bak", "Export/Track 01/project.o3pj", "Export/Track 01/sub/a.wav", "Export/a/b/c/d",
                           "Exports/Track 01/a.wav", "Exportx", "xExport/a"}) {
    INFO("path: " << none);
    CHECK(sv::classify(none) == Place::None);
  }
  // Operations: the folders may be made, a Temp created and removed, a Target only appears by a rename of the Temp and is then never touched again.
  CHECK(sv::may_make_dir("Export"));
  CHECK(sv::may_make_dir("Export/Track 01"));
  CHECK_FALSE(sv::may_make_dir("Export/Track 01/Stems"));
  CHECK_FALSE(sv::may_make_dir("Export/Track 01/a.wav"));
  CHECK(sv::may_create("Export/Track 01/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("Export/Track 01/a.wav"));
  CHECK(sv::may_remove("Export/Track 01/_o3save.tmp"));
  CHECK_FALSE(sv::may_remove("Export/Track 01/a.wav"));
  CHECK_FALSE(sv::may_remove("Export/Track 01"));
  CHECK_FALSE(sv::may_remove("Export"));
  CHECK(sv::may_rename("Export/Track 01/_o3save.tmp", "Export/Track 01/Track 01_Song0001.wav"));
  CHECK_FALSE(sv::may_rename("Export/Track 01/Track 01_Song0001.wav", "Export/Track 01/Track 01_Song0002.wav"));
  CHECK_FALSE(sv::may_rename("Export/Track 01/Track 01_Song0001.wav", "Export/Track 01/Track 01_Song0001.wav.bak"));
  CHECK_FALSE(sv::may_rename("Export/Track 01/_o3save.tmp", "Export/Track 02/a.wav"));  // another folder
  CHECK_FALSE(sv::may_rename("Export/Track 01/_o3save.tmp", "Samples/a.wav"));
  CHECK_FALSE(sv::may_rename("Samples/_o3save.tmp", "Export/Track 01/a.wav"));
}

TEST_CASE("save guard: Export/ takes its .wav and instruments/ its .pti, each refuses the other's, both refuse traversal, and no folder is ever removed (OS-WAVEXP + OS-INSTUI S7)") {
  // Export/<project>/<stem>.wav
  CHECK(sv::classify("Export") == Place::Folder);
  CHECK(sv::classify("Export/Song") == Place::Folder);
  CHECK(sv::classify("export/song/Song_Song0001.WAV") == Place::Target);
  CHECK(sv::classify("Export/Song/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify("Export/Song/x.pti") == Place::None);
  CHECK(sv::classify("Export/x.wav") == Place::Folder);  // (the second component is the project's folder, whatever its name: a file is never made there, may_create takes a Temp only)
  CHECK(sv::classify("Export/x.wav/x.pti") == Place::None);
  // instruments/<stem>.pti
  CHECK(sv::classify("instruments") == Place::Folder);
  CHECK(sv::classify("instruments/KICK.pti") == Place::Target);
  CHECK(sv::classify("instruments/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify("instruments/KICK.wav") == Place::None);
  CHECK(sv::classify("instruments/Song/KICK.pti") == Place::None);  // no sub-folders
  // traversal and look-alikes
  for (const char* none : {"Export/../Projects/a/project.o3pj", "Export/Song/../x.wav", "Export/./x/y.wav", "Export/Song/../../instruments/x.pti", "instruments/../Export/Song/x.wav",
                           "instruments/../x.pti", "instruments/./x.pti", "instruments/..", "Export/..", "instruments/x.pti/..", "Export/Song/x.wav/..", "instruments//x.pti", "Export//Song"}) {
    INFO("path: " << none);
    CHECK(sv::classify(none) == Place::None);
  }
  // no Backup form in either: nothing but a Temp is ever removed there, and no folder is
  for (const char* path : {"Export", "Export/Song", "instruments", "Export/Song/Song_Song0001.wav", "instruments/KICK.pti", "Export/Song/Song_Song0001.wav.bak", "instruments/KICK.pti.bak"}) {
    INFO("path: " << path);
    CHECK_FALSE(sv::may_remove(path));
    CHECK_FALSE(sv::may_create(path));
  }
  CHECK(sv::may_remove("Export/Song/_o3save.tmp"));
  CHECK(sv::may_remove("instruments/_o3save.tmp"));
  // a Temp becomes a Target of its own folder's kind only
  CHECK(sv::may_rename("Export/Song/_o3save.tmp", "Export/Song/Song_Song0001.wav"));
  CHECK(sv::may_rename("instruments/_o3save.tmp", "instruments/KICK.pti"));
  CHECK_FALSE(sv::may_rename("Export/Song/_o3save.tmp", "Export/Song/KICK.pti"));
  CHECK_FALSE(sv::may_rename("instruments/_o3save.tmp", "instruments/KICK.wav"));
  CHECK_FALSE(sv::may_rename("instruments/_o3save.tmp", "Export/Song/Song_Song0001.wav"));
  CHECK_FALSE(sv::may_rename("Export/Song/_o3save.tmp", "instruments/KICK.pti"));
  CHECK_FALSE(sv::may_rename("instruments/KICK.pti", "instruments/KICK.pti.bak"));
  CHECK_FALSE(sv::may_rename("Export/Song/Song_Song0001.wav", "Export/Song/Song_Song0001.wav.bak"));
  CHECK_FALSE(sv::may_rename("Export/Song", "Export/Other"));
  CHECK_FALSE(sv::may_rename("instruments", "Instruments2"));
  // folders may be made, and only those
  CHECK(sv::may_make_dir("Export"));
  CHECK(sv::may_make_dir("Export/Song"));
  CHECK(sv::may_make_dir("instruments"));
  CHECK_FALSE(sv::may_make_dir("instruments/sub"));
  CHECK_FALSE(sv::may_make_dir("Export/Song/sub"));
  CHECK_FALSE(sv::may_make_dir("Export/Song/x.wav"));
  CHECK_FALSE(sv::may_make_dir("instruments/x.pti"));
}

// ---- OS-FILES part B: Settings/recent.o3rc, the RENAME of a project folder, the .deleted marker -------------------------------------------------------------------------------------------

TEST_CASE("save guard: Settings/recent.o3rc is a Target with a Backup, replaced the way the settings are (OS-FILES part B)") {
  CHECK(sv::classify("Settings/recent.o3rc") == Place::Target);
  CHECK(sv::classify("SETTINGS/RECENT.O3RC.BAK") == Place::Backup);
  CHECK(sv::classify("Settings/recent.o3rc.tmp") == Place::None);
  CHECK(sv::classify("Settings/recent.o3rc.bak.bak") == Place::None);
  CHECK(sv::classify("Settings/recent.o3rc/x") == Place::None);
  CHECK(sv::may_rename("Settings/_o3save.tmp", "Settings/recent.o3rc"));
  CHECK(sv::may_rename("Settings/recent.o3rc", "Settings/recent.o3rc.bak"));
  CHECK(sv::may_rename("Settings/recent.o3rc.bak", "Settings/recent.o3rc"));
  CHECK_FALSE(sv::may_rename("Settings/recent.o3rc", "Settings/device.o3st.bak"));  // a backup is of its own target
  CHECK_FALSE(sv::may_rename("Settings/device.o3st", "Settings/recent.o3rc.bak"));
  CHECK(sv::may_remove("Settings/recent.o3rc.bak"));
  CHECK_FALSE(sv::may_remove("Settings/recent.o3rc"));
  CHECK_FALSE(sv::may_create("Settings/recent.o3rc"));
  // There is no Trash: it is not a guarded place (the cross-folder moves it was for are gone, docs/specs/OS-FILES-B.md)
  for (const char* none : {"Trash", "Trash/Song", "Trash/Song_SAMPLES/instr01.wav"}) {
    CHECK(sv::classify(none) == Place::None);
    CHECK_FALSE(sv::may_make_dir(none));
  }
}

TEST_CASE("save guard: a project folder is never renamed: not inside Projects/, not to another folder, not by case (OS-FILES part B dropped RENAME)") {
  const std::string longest = "Projects/" + std::string(sv::kNameMax, 'b');
  for (const char* to : {"Projects/Song 2", "Projects/Other-Name_1.v2", "Projects/Song", "Projects/SONG", "Projects/song", "Projects/a", longest.c_str(), "Samples/Song",
                         "Settings/Song", "Export/Song", "instruments/Song", "Song", "Projects/Song/Song", "Projects/../Song", "Projects/../Samples", "Trash/Song", "Trash/Song_2", "Projects", "Trash"}) {
    INFO("target: " << to);
    CHECK_FALSE(sv::may_rename("Projects/Song", to));
  }
  CHECK_FALSE(sv::may_rename("projects/SONG", "PROJECTS/Other"));
  CHECK_FALSE(sv::may_rename("Projects/a", "Projects/b"));
  for (const char* from : {"Samples", "Export/Song", "Settings", "Projects", "Trash/Song", "instruments"}) {
    INFO("source: " << from);
    CHECK_FALSE(sv::may_rename(from, "Projects/Song"));
    CHECK_FALSE(sv::may_rename(from, "Projects/New"));
  }
  CHECK_FALSE(sv::may_rename("Export/Song", "Export/Other"));
  CHECK_FALSE(sv::may_rename("Projects/Song", nullptr));
  CHECK_FALSE(sv::may_rename(nullptr, "Projects/Song"));
  // the project's own files and folders do not move either, and no file of a project goes anywhere else
  for (const char* from : {"Projects/Song/project.o3pj", "Projects/Song/samples", "Projects/Song/samples/instr01.wav", "Projects/Song/patterns", "Projects/Song/.deleted", "Projects/Song/project.o3pj.bak"}) {
    for (const char* to : {"Projects/Other/project.o3pj", "Projects/Other/samples", "Trash/Song/x.wav", "Projects/Song/other", "Samples/x.wav"}) {
      INFO(from << " -> " << to);
      CHECK_FALSE(sv::may_rename(from, to));
    }
  }
  // what is still allowed is only what the save layer does: Temp -> Target, Target -> its Backup, Backup -> its Target
  CHECK(sv::may_rename("Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj"));
  CHECK(sv::may_rename("Projects/Song/project.o3pj", "Projects/Song/project.o3pj.bak"));
}

TEST_CASE("save guard: the .deleted marker is a created-new empty file directly inside a project folder, and nothing else (OS-FILES part B)") {
  CHECK(sv::classify("Projects/Song/.deleted") == Place::Marker);
  CHECK(sv::classify("PROJECTS/song/.DELETED") == Place::Marker);
  CHECK(sv::may_create_marker("Projects/Song/.deleted"));
  CHECK(sv::may_create_marker("projects/Track 01/.Deleted"));
  CHECK(sv::may_create_marker((std::string("Projects/") + std::string(sv::kNameMax, 'x') + "/.deleted").c_str()));
  for (const char* bad : {"Projects/.deleted", ".deleted", "Projects/Song/.deleted/x", "Projects/Song/.deleted.bak", "Projects/Song/.deleted2", "Projects/Song/sub/.deleted", "Projects/Song/samples/.deleted",
                          "Projects/Song/deleted", "Projects/Song/.delete", "Projects/Song/ .deleted", "Projects/CON/.deleted", "Projects/ lead/.deleted", "Projects/../.deleted", "Projects/Song/../.deleted",
                          "Samples/.deleted", "Samples/Song/.deleted", "Export/Song/.deleted", "instruments/.deleted", "Settings/.deleted", "Trash/Song/.deleted", "Projects/Song//.deleted", "Projects/Song/.deleted/"}) {
    INFO("path: " << bad);
    CHECK_FALSE(sv::may_create_marker(bad));
  }
  CHECK_FALSE(sv::may_create_marker(nullptr));
  // it is not a Temp, a Target or a Backup: no other call touches it
  const char* marker = "Projects/Song/.deleted";
  CHECK_FALSE(sv::may_create(marker));      // create() is for Temps: nothing is ever written to it
  CHECK_FALSE(sv::may_remove(marker));      // never removed by this layer (removing it is the user's way back)
  CHECK_FALSE(sv::may_make_dir(marker));
  CHECK_FALSE(sv::may_rename(marker, "Projects/Song/.deleted.bak"));
  CHECK_FALSE(sv::may_rename("Projects/Song/_o3save.tmp", marker));  // a Temp does not become it
  CHECK_FALSE(sv::may_rename("Projects/Song/project.o3pj", marker));
  CHECK_FALSE(sv::may_rename(marker, "Projects/Song/project.o3pj"));
  // and the marker call takes no other file
  for (const char* other : {"Projects/Song/project.o3pj", "Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj.bak", "Projects/Song/samples/instr01.wav", "Samples/REC_001.wav", "Settings/device.o3st",
                            "Settings/recent.o3rc", "Projects/Song", "Projects"}) {
    INFO("path: " << other);
    CHECK_FALSE(sv::may_create_marker(other));
  }
  // the folder of a project that holds the marker is still a folder the guard knows
  CHECK(sv::classify("Projects/Song") == Place::Folder);
}

TEST_CASE("save guard: may_remove is what it was: a Temp or a Backup, never a Target, a Marker, a folder or the user's file (OS-FILES part B)") {
  for (const char* path : {"Projects", "Projects/Song", "Projects/Song/project.o3pj", "Projects/Song/.deleted", "Projects/Song/samples", "Projects/Song/samples/instr01.wav", "Samples", "Samples/REC_001.wav", "Settings",
                           "Settings/device.o3st", "Settings/recent.o3rc", "Export", "Export/Song", "Export/Song/Song_Song0001.wav", "instruments", "instruments/KICK.pti", "Projects/Song/project.mt",
                           "Projects/Song/patterns/pattern_01.mtp"}) {
    INFO("path: " << path);
    CHECK_FALSE(sv::may_remove(path));
  }
  for (const char* path : {"Projects/Song/_o3save.tmp", "Projects/Song/project.o3pj.bak", "Projects/Song/samples/_o3save.tmp", "Projects/Song/samples/instr01.wav.bak", "Settings/_o3save.tmp",
                           "Settings/device.o3st.bak", "Settings/recent.o3rc.bak", "Export/Song/_o3save.tmp", "instruments/_o3save.tmp", "Samples/_o3save.tmp"}) {
    INFO("path: " << path);
    CHECK(sv::may_remove(path));
  }
}

// ---- OS-H4: AppExport/<app>/<stem>.<ext>, the files an SD app module writes (ADR 0014 amendment A) --------------------------------------------------------------------------------------

// The guard is the last line: whatever the export service above it does, these are the only paths an app's file can ever have, and the only calls that can touch them.
static_assert(sv::classify("AppExport") == Place::Folder, "AppExport is a folder");
static_assert(sv::classify("AppExport/smfexport") == Place::Folder, "AppExport/<app> is a folder");
static_assert(sv::classify("AppExport/smfexport/PAT001.mid") == Place::Target, "a file of the allowlist is a Target");
static_assert(sv::classify("AppExport/smfexport/PAT001.ptf") == Place::None, "a firmware image can never come from an app");
static_assert(sv::classify("AppExport/smfexport/_o3save.tmp") == Place::Temp, "the Temp");
static_assert(sv::may_rename("AppExport/a/_o3save.tmp", "AppExport/a/x.mid") && !sv::may_remove("AppExport/a/x.mid"), "a Target appears by a rename of the Temp and is never removed");

static_assert(sv::app_ext_ok("mid") && sv::app_ext_ok("syx") && sv::app_ext_ok("txt") && sv::app_ext_ok("csv") &&
                  sv::app_ext_ok("json"),
              "the allowlist");
static_assert(!sv::app_ext_ok(nullptr) && !sv::app_ext_ok("") && !sv::app_ext_ok("MID") && !sv::app_ext_ok(".mid") &&
                  !sv::app_ext_ok("mid.ptf") && !sv::app_ext_ok("mi") && !sv::app_ext_ok("ptf") && !sv::app_ext_ok("tmod") &&
                  !sv::app_ext_ok("o3pj") && !sv::app_ext_ok("wav"),
              "what an app may ask for is lower case and on the list, nothing else");

TEST_CASE("save guard OS-H4: every allowlisted extension is a Target in any case, every other one is refused") {
  CHECK(sv::kAppExportExtCount == 5u);
  for (const char* ext : {"mid", "syx", "txt", "csv", "json", "MID", "Syx", "TXT", "Csv", "JSON"}) {
    const std::string path = std::string("AppExport/smfexport/PAT001.") + ext;
    INFO("path: " << path);
    CHECK(sv::classify(path.c_str()) == Place::Target);
    CHECK_FALSE(sv::may_create(path.c_str()));   // never written to directly
    CHECK_FALSE(sv::may_remove(path.c_str()));   // never removed
    CHECK(sv::may_rename("AppExport/smfexport/_o3save.tmp", path.c_str()));
    CHECK_FALSE(sv::may_rename(path.c_str(), "AppExport/smfexport/other.mid"));  // never renamed
    CHECK_FALSE(sv::may_rename(path.c_str(), (path + ".bak").c_str()));
    CHECK_FALSE(sv::may_rename("AppExport/smfexport/other.mid", path.c_str()));
  }
  for (const char* ext : {"ptf", "PTF", "tmod", "o3pj", "wav", "pti", "bak", "tmp", "o3st", "o3rc", "mti", "mt", "mp3", "midi", "mid ", "m", "jso", "jsonx", "txt.ptf", "mid.ptf", "mid.bak", "", ".", "mid."}) {
    const std::string path = std::string("AppExport/smfexport/PAT001.") + ext;
    INFO("path: " << path);
    CHECK(sv::classify(path.c_str()) == Place::None);
    CHECK_FALSE(sv::may_rename("AppExport/smfexport/_o3save.tmp", path.c_str()));
    CHECK_FALSE(sv::may_create(path.c_str()));
    CHECK_FALSE(sv::may_remove(path.c_str()));
  }
  // no extension at all, only an extension, an empty stem
  for (const char* none : {"AppExport/smfexport/PAT001", "AppExport/smfexport/mid", "AppExport/smfexport/.mid", "AppExport/smfexport/..mid", "AppExport/smfexport/ .mid", "AppExport/smfexport/a..mid",
                           "AppExport/smfexport/a.mid.ptf", "AppExport/smfexport/a.ptf.mid.ptf", "AppExport/smfexport/trail .mid", "AppExport/smfexport/trail..mid", "AppExport/smfexport/-lead.mid",
                           "AppExport/smfexport/CON.mid", "AppExport/smfexport/nul.txt", "AppExport/smfexport/COM1.csv", "AppExport/smfexport/LPT9.json", "AppExport/smfexport/a:b.mid", "AppExport/smfexport/a*b.mid",
                           "AppExport/smfexport/caf\xC3\xA9.mid"}) {
    INFO("path: " << none);
    CHECK(sv::classify(none) == Place::None);
  }
  // names that are fine
  for (const char* ok : {"AppExport/smfexport/a.mid", "AppExport/smfexport/My Song_1-2.mid", "AppExport/smfexport/a.b.c.mid", "AppExport/smfexport/CONSOLE.mid", "AppExport/smfexport/PAT001_SEL_001.mid", "AppExport/smfexport/9.json"}) {
    INFO("path: " << ok);
    CHECK(sv::classify(ok) == Place::Target);
  }
}

TEST_CASE("save guard OS-H4: the folders, the Temp, the 31-byte limits and the longest path") {
  CHECK(sv::classify("AppExport") == Place::Folder);
  CHECK(sv::classify("APPEXPORT") == Place::Folder);
  CHECK(sv::classify("appexport/SMFEXPORT") == Place::Folder);
  CHECK(sv::classify("AppExport/smfexport/_O3SAVE.TMP") == Place::Temp);
  CHECK(sv::kAppExportDir == std::string("AppExport"));
  const std::string name(sv::kNameMax, 'x');
  const std::string longest = std::string("AppExport/") + name + "/" + name + ".json";
  CHECK(longest.size() + 1 <= sv::kPathCap);
  CHECK(sv::classify(longest.c_str()) == Place::Target);
  CHECK(sv::classify((std::string("AppExport/") + name + "/_o3save.tmp").c_str()) == Place::Temp);
  CHECK(sv::classify((std::string("AppExport/") + name + "x/a.mid").c_str()) == Place::None);   // an app folder of 32 bytes
  CHECK(sv::classify((std::string("AppExport/") + name + "x").c_str()) == Place::None);
  CHECK(sv::classify((std::string("AppExport/a/") + name + "x.mid").c_str()) == Place::None);  // a stem of 32 bytes
  char out[sv::kPathCap];
  CHECK(sv::join(out, sizeof out, "AppExport", name.c_str(), (name + ".json").c_str()));
  CHECK(sv::classify(out) == Place::Target);
  // Operations on the folders: made, never removed or renamed, no file made in their place.
  CHECK(sv::may_make_dir("AppExport"));
  CHECK(sv::may_make_dir("AppExport/smfexport"));
  CHECK_FALSE(sv::may_make_dir("AppExport/smfexport/sub"));
  CHECK_FALSE(sv::may_make_dir("AppExport/smfexport/a.mid"));
  CHECK_FALSE(sv::may_make_dir("AppExport/CON"));
  CHECK_FALSE(sv::may_make_dir("AppExport/ lead"));
  CHECK_FALSE(sv::may_remove("AppExport"));
  CHECK_FALSE(sv::may_remove("AppExport/smfexport"));
  CHECK_FALSE(sv::may_create("AppExport"));
  CHECK_FALSE(sv::may_create("AppExport/smfexport"));
  CHECK_FALSE(sv::may_rename("AppExport/smfexport", "AppExport/other"));
  CHECK_FALSE(sv::may_rename("AppExport", "Export"));
  // The Temp: created and removed (an abort or a failure), and only it.
  CHECK(sv::may_create("AppExport/smfexport/_o3save.tmp"));
  CHECK(sv::may_remove("AppExport/smfexport/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("AppExport/_o3save.tmp"));  // not in the root of AppExport: that level holds app folders only
  CHECK_FALSE(sv::may_remove("AppExport/_o3save.tmp"));
  CHECK(sv::classify("AppExport/_o3save.tmp") == Place::None);  // (valid_name refuses a leading '_', so it is not an app folder either)
}

TEST_CASE("save guard OS-H4: '..', '/' and '\\' in the app or in the stem, and anything deeper than the app's folder") {
  for (const char* none : {"AppExport/..", "AppExport/.", "AppExport/../Projects/a/project.o3pj", "AppExport/smfexport/../x.mid", "AppExport/../x.mid", "AppExport/smfexport/..", "AppExport/..//x.mid",
                           "AppExport//smfexport/a.mid", "AppExport/smfexport//a.mid", "AppExport/smfexport/a.mid/", "/AppExport/smfexport/a.mid", "AppExport/smfexport/a\\b.mid", "AppExport\\smfexport\\a.mid",
                           "AppExport/a\\b/c.mid", "AppExport/smfexport/../../Projects/a/project.o3pj", "AppExport/smfexport/a/b.mid", "AppExport/smfexport/sub/a.mid", "AppExport/smfexport/sub/_o3save.tmp",
                           "AppExport/a/b/c/d.mid", "AppExport/smfexport/a.mid/b.mid", "AppExport/smfexport/_o3save.tmp/x.mid", "AppExport/smfexport/_o3save.tmp.bak", "AppExport/smfexport/_o3save.tmp.mid",
                           "AppExportx/smfexport/a.mid", "xAppExport/smfexport/a.mid", "AppExpor/smfexport/a.mid", "App Export/smfexport/a.mid", "Export/../AppExport/a/b.mid"}) {
    INFO("path: " << none);
    const Place p = sv::classify(none);
    CHECK((p == Place::None || p == Place::Folder));  // "AppExport/.." is not a folder name: None; none of these is a Target, a Temp or a Backup
    CHECK(p != Place::Target);
    CHECK(p != Place::Temp);
    CHECK(p != Place::Backup);
    CHECK_FALSE(sv::may_create(none));
    CHECK_FALSE(sv::may_remove(none));
  }
  CHECK(sv::classify("AppExport/..") == Place::None);
  CHECK(sv::classify("AppExport/.") == Place::None);
  CHECK(sv::classify("AppExport/smfexport/a/b.mid") == Place::None);
  CHECK(sv::classify("AppExport/smfexport/sub/_o3save.tmp") == Place::None);
  // rename: both ends in the same app folder, nothing across, no traversal
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "AppExport/b/x.mid"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "AppExport/a/../b/x.mid"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "AppExport/x.mid"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "AppExport/a/sub/x.mid"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "Export/a/x.wav"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "Samples/x.wav"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "Projects/x/project.o3pj"));
  CHECK_FALSE(sv::may_rename("Export/a/_o3save.tmp", "AppExport/a/x.mid"));
  CHECK_FALSE(sv::may_rename("Samples/_o3save.tmp", "AppExport/a/x.mid"));
  CHECK_FALSE(sv::may_rename("AppExport/a/_o3save.tmp", "AppExport/a/_o3save.tmp"));
  CHECK_FALSE(sv::may_rename("AppExport/a/x.mid", "AppExport/a/_o3save.tmp"));  // a finished file never goes back to a Temp
}

TEST_CASE("save guard OS-H4: an app's file has no Backup form, so a finished file can be used as a source of nothing") {
  // The three things a save may do to a finished file are all refused: rename it (to a Backup, a Temp, another Target), remove it, create over it.
  const char* file = "AppExport/smfexport/PAT001.mid";
  for (const char* to : {"AppExport/smfexport/PAT001.mid.bak", "AppExport/smfexport/_o3save.tmp", "AppExport/smfexport/PAT002.mid", "AppExport/smfexport/PAT001.mid", "AppExport/other/PAT001.mid",
                         "Samples/PAT001.wav", "Settings/device.o3st", "Projects/PAT001/project.o3pj"}) {
    INFO("to: " << to);
    CHECK_FALSE(sv::may_rename(file, to));
  }
  CHECK_FALSE(sv::may_remove(file));
  CHECK_FALSE(sv::may_create(file));
  CHECK_FALSE(sv::may_remove("AppExport/smfexport/PAT001.mid.bak"));  // no such place
  CHECK(sv::classify("AppExport/smfexport/PAT001.mid.bak") == Place::None);
  // the other kinds' backups cannot be made out of an app's file or into its folder
  CHECK_FALSE(sv::may_rename("AppExport/smfexport/project.o3pj", "AppExport/smfexport/project.o3pj.bak"));
  CHECK_FALSE(sv::may_rename("AppExport/smfexport/_o3save.tmp", "AppExport/smfexport/project.o3pj"));
  CHECK_FALSE(sv::may_remove("AppExport/smfexport/project.o3pj.bak"));
  CHECK_FALSE(sv::may_remove("AppExport/smfexport/device.o3st.bak"));
  // the marker is the project folder's alone
  CHECK_FALSE(sv::may_create_marker("AppExport/smfexport/.deleted"));
  CHECK_FALSE(sv::may_create_marker("AppExport/.deleted"));
  CHECK_FALSE(sv::may_create_marker("AppExport/smfexport/a.mid"));
}

TEST_CASE("save guard OS-H4: the older places are exactly what they were") {
  // Adding AppExport changed nothing about the paths of the other kinds (a spot check of each kind; the older cases above walk them exhaustively).
  CHECK(sv::classify("Export/Song/Song_Song0001.wav") == Place::Target);
  CHECK(sv::classify("Export/Song/a.mid") == Place::None);  // the WAV export's folder takes no .mid
  CHECK(sv::classify("Samples/a.mid") == Place::None);
  CHECK(sv::classify("Projects/x/a.mid") == Place::None);
  CHECK(sv::classify("instruments/a.mid") == Place::None);
  CHECK(sv::classify("Settings/a.mid") == Place::None);
  CHECK(sv::classify("Projects/x/project.o3pj.bak") == Place::Backup);
  CHECK(sv::classify("Settings/device.o3st.bak") == Place::Backup);
}

// ---- OS-FILES-C C2a (T7): a whole project in the OG format, Export/<name> OG[ n] -----------------------------------------------------------------------------------------------------

TEST_CASE("save guard OS-FILES-C T7: the OG export folder shape is <name> OG, then <name> OG 2 .. 99, and nothing else") {
  for (const char* yes : {"Song OG", "Song OG 2", "Song OG 9", "Song OG 10", "Song OG 99", "a OG", "A OG 99", "Track 01 OG", "My-Song_2.v3 OG 7", "song og", "Song Og 5", "x. y OG", "OG OG", "OG OG 2",
                          "Song OG 5 OG", "Song OG 5 OG 6"}) {
    INFO(yes);
    CHECK(sv::og_export_folder(yes));
    CHECK(sv::classify((std::string("Export/") + yes).c_str()) == Place::Folder);
  }
  // 25 bytes of name plus " OG 99" is the 31 the guard takes; one more is not a name at all
  CHECK(sv::og_export_folder((std::string(sv::kOgNameRoom, 'x') + " OG 99").c_str()));
  CHECK((std::string(sv::kOgNameRoom, 'x') + " OG 99").size() == sv::kNameMax);
  CHECK_FALSE(sv::og_export_folder((std::string(sv::kOgNameRoom + 1, 'x') + " OG 99").c_str()));
  for (const char* no : {"Song", "Song OG 1", "Song OG 0", "Song OG 100", "Song OG 02", "Song OG 00", "Song OG  2", "Song  OG", "Song  OG 2", "SongOG", "Song OG2", "Song OG 2 ", "Song OG ", "Song-OG",
                         "Song_OG", "Song OG 2x", "Song OG x", "Song OG -2", "Song OG 2.5", "Song OG 123", "Song O G", "OG", " OG", "OG 2", "Song.OG", "Song. OG", "Song OG.", "Song OGx", "xOG 2", "Song 2 OG 1",
                         "Song OG 9 9", "con", "a/b OG", "a..b OG", "OG\t2"}) {
    INFO(no);
    CHECK_FALSE(sv::og_export_folder(no));
  }
  CHECK_FALSE(sv::og_export_folder(nullptr));
  CHECK_FALSE(sv::og_export_folder(""));
  // the near-miss names are still plain Export/<name> folders (the stems' folders), but hold none of the OG's files
  for (const char* plain : {"Song", "Song OG 1", "Song OG 100", "Song OG 02", "Song OG  2", "OG"}) {
    INFO(plain);
    const std::string base = std::string("Export/") + plain;
    CHECK(sv::classify(base.c_str()) == (sv::valid_name(plain) ? Place::Folder : Place::None));
    for (const char* inner : {"/project.mt", "/patterns", "/instruments", "/patterns/pattern_01.mtp", "/patterns/patternsMetadata", "/instruments/01 Kick.pti", "/patterns/_o3save.tmp", "/instruments/_o3save.tmp"}) {
      INFO(inner);
      CHECK(sv::classify((base + inner).c_str()) == Place::None);
    }
  }
}

TEST_CASE("save guard OS-FILES-C T7: the OG files, each in its exact place, in any case, and every near miss refused") {
  const std::string og = "Export/Song OG";
  for (const std::string& base : {og, std::string("Export/Song OG 2"), std::string("Export/Song OG 99"), std::string("EXPORT/SONG OG 7")}) {
    INFO(base);
    CHECK(sv::classify(base.c_str()) == Place::Folder);
    CHECK(sv::classify((base + "/patterns").c_str()) == Place::Folder);
    CHECK(sv::classify((base + "/Patterns").c_str()) == Place::Folder);
    CHECK(sv::classify((base + "/instruments").c_str()) == Place::Folder);
    CHECK(sv::classify((base + "/project.mt").c_str()) == Place::Target);
    CHECK(sv::classify((base + "/PROJECT.MT").c_str()) == Place::Target);
    CHECK(sv::classify((base + "/_o3save.tmp").c_str()) == Place::Temp);
    CHECK(sv::classify((base + "/patterns/_o3save.tmp").c_str()) == Place::Temp);
    CHECK(sv::classify((base + "/instruments/_o3save.tmp").c_str()) == Place::Temp);
    CHECK(sv::classify((base + "/patterns/patternsMetadata").c_str()) == Place::Target);
    CHECK(sv::classify((base + "/patterns/PATTERNSMETADATA").c_str()) == Place::Target);
    for (const char* p : {"01", "02", "09", "10", "42", "99", "100", "128", "199", "200", "255"}) {
      INFO(p);
      CHECK(sv::classify((base + "/patterns/pattern_" + p + ".mtp").c_str()) == Place::Target);
    }
    for (const char* i : {"01 Kick.pti", "48 Snare 2.pti", "09 a.pti", "12 Kick.PTI", "01 x.pti"}) {
      INFO(i);
      CHECK(sv::classify((base + "/instruments/" + i).c_str()) == Place::Target);
    }
  }
  // the longest path fits the path buffer
  const std::string longest = "Export/" + std::string(sv::kOgNameRoom, 'x') + " OG 99/instruments/48 " + std::string(sv::kNameMax - 3, 'y') + ".pti";
  CHECK(sv::classify(longest.c_str()) == Place::Target);
  CHECK(longest.size() + 1u <= sv::kPathCap);
  // near misses: the OG's file names where they do not belong
  const std::string stems = "Export/Song";
  for (const std::string& folder : {stems, std::string("Export/Song OG 1"), std::string("Export/Song OG 100"), std::string("Export/Song OG 02"), std::string("Export/Song OG  2"), std::string("Export/OG"),
                                     std::string("Projects/Song OG"), std::string("Projects/Song"), std::string("instruments"), std::string("Samples"), std::string("Settings"), std::string("AppExport/Song OG")}) {
    INFO(folder);
    for (const char* inner : {"/project.mt", "/patterns", "/instruments", "/patterns/pattern_01.mtp", "/patterns/patternsMetadata", "/instruments/01 Kick.pti", "/patterns/_o3save.tmp", "/instruments/_o3save.tmp",
                              "/sub/project.mt"}) {
      INFO(inner);
      const Place p = sv::classify((folder + inner).c_str());
      CHECK(p != Place::Target);
      CHECK(p != Place::Backup);
      CHECK(p != Place::Marker);
      if (std::string(inner).find("_o3save.tmp") != std::string::npos && std::string(inner).find("patterns") != std::string::npos) {
        CHECK(p == Place::None);  // no temp one level down outside an OG folder
      }
    }
  }
  for (const char* no : {
           // project.mt
           "Export/Song OG/patterns/project.mt", "Export/Song OG/instruments/project.mt", "Export/Song OG/project.mt.bak", "Export/Song OG/project.mt.tmp", "Export/Song OG/project.mtx", "Export/Song OG/project_mt",
           "Export/Song OG/project.mt/x", "Export/Song OG/Project.o3pj", "Export/Song OG/sub/project.mt", "Projects/Song OG/project.mt", "Export/project.mt", "project.mt", "Samples/project.mt",
           // folders
           "Export/Song OG/sub", "Export/Song OG/patterns/sub", "Export/Song OG/patterns/old", "Export/Song OG/instruments/sub", "Export/Song OG/samples", "Export/Song OG/patterns/x/pattern_01.mtp",
           "Export/Song OG/patterns/sub/_o3save.tmp", "Export/Song OG/instruments/sub/_o3save.tmp", "Export/Song OG/sub/_o3save.tmp", "Export/Song OG/patterns/instruments",
           // patterns
           "Export/Song OG/patterns/pattern_0.mtp", "Export/Song OG/patterns/pattern_1.mtp", "Export/Song OG/patterns/pattern_9.mtp", "Export/Song OG/patterns/pattern_00.mtp",
           "Export/Song OG/patterns/pattern_000.mtp", "Export/Song OG/patterns/pattern_001.mtp", "Export/Song OG/patterns/pattern_099.mtp", "Export/Song OG/patterns/pattern_256.mtp",
           "Export/Song OG/patterns/pattern_300.mtp", "Export/Song OG/patterns/pattern_1000.mtp", "Export/Song OG/patterns/pattern_.mtp", "Export/Song OG/patterns/pattern_ab.mtp",
           "Export/Song OG/patterns/pattern_1a.mtp", "Export/Song OG/patterns/pattern_-1.mtp", "Export/Song OG/patterns/pattern_ 1.mtp", "Export/Song OG/patterns/pattern_01.mtp.bak",
           "Export/Song OG/patterns/pattern_01.mtp.tmp", "Export/Song OG/patterns/pattern_01.mt", "Export/Song OG/patterns/pattern_01.mtpx", "Export/Song OG/patterns/pattern_01", "Export/Song OG/patterns/pattern01.mtp",
           "Export/Song OG/patterns/Pattern 01.mtp", "Export/Song OG/patterns/._pattern_01.mtp", "Export/Song OG/pattern_01.mtp", "Export/Song OG/instruments/pattern_01.mtp",
           "Export/Song OG/patterns/instrument_01.mti", "Export/Song OG/patterns/01 Kick.pti", "Export/Song/patterns/pattern_01.mtp", "Export/Song OG 1/patterns/pattern_01.mtp",
           // patternsMetadata
           "Export/Song OG/patternsMetadata", "Export/Song OG/instruments/patternsMetadata", "Export/Song OG/patterns/patternsMetadata.bak", "Export/Song OG/patterns/patternsMetadat",
           "Export/Song OG/patterns/patternsMetadata2", "Export/Song OG/patterns/patterns Metadata", "Export/Song OG/patterns/patternsMetadata/x", "Export/Song/patterns/patternsMetadata",
           // instruments
           "Export/Song OG/instruments/00 Kick.pti", "Export/Song OG/instruments/49 Kick.pti", "Export/Song OG/instruments/99 Kick.pti", "Export/Song OG/instruments/1 Kick.pti",
           "Export/Song OG/instruments/001 Kick.pti", "Export/Song OG/instruments/01Kick.pti", "Export/Song OG/instruments/01_Kick.pti", "Export/Song OG/instruments/01 .pti", "Export/Song OG/instruments/01.pti",
           "Export/Song OG/instruments/Kick.pti", "Export/Song OG/instruments/instrument_01.pti", "Export/Song OG/instruments/instrument_01.mti", "Export/Song OG/instruments/01 Kick.pti.bak",
           "Export/Song OG/instruments/01 Kick.mti", "Export/Song OG/instruments/01 Kick.wav", "Export/Song OG/instruments/01 Kick..pti", "Export/Song OG/instruments/01 Kick .pti",
           "Export/Song OG/instruments/01 a/b.pti", "Export/Song OG/instruments/01 a..b.pti", "Export/Song OG/instruments/01 caf\xC3\xA9.pti", "Export/Song OG/instruments/01 a(b).pti", "Export/Song OG/instruments/01 a&b.pti",
           "Export/Song OG/patterns/01 Kick.pti", "Export/Song OG/01 Kick.pti", "instruments/01 Kick.mti", "Export/Song OG/instruments/01 Kick.pti/x",
           // five components, traversal, empties
           "Export/Song OG/patterns/pattern_01.mtp/x", "Export/Song OG/patterns/sub/pattern_01.mtp", "Export/Song OG/../Song OG/project.mt", "Export/Song OG/patterns/../project.mt",
           "Export/Song OG/./project.mt", "Export/Song OG//project.mt", "Export/Song OG/patterns//pattern_01.mtp", "Export/Song OG/", "Export/Song OG/patterns/", "/Export/Song OG/project.mt",
           "Export/../Export/Song OG/project.mt"}) {
    INFO(no);
    CHECK(sv::classify(no) != Place::Target);
    CHECK(sv::classify(no) != Place::Backup);
    CHECK(sv::classify(no) != Place::Temp);
    CHECK_FALSE(sv::may_create(no));
    CHECK_FALSE(sv::may_remove(no));
  }
  // paths with no meaning at all are None, not a folder
  for (const char* none : {"Export/Song OG/patterns/pattern_0.mtp", "Export/Song OG/patterns/sub", "Export/Song OG/sub", "Export/Song OG/patterns/x/y", "Export/Song OG/patterns/pattern_01.mtp/x", "Export/Song OG/instruments/sub",
                           "Export/Song OG/project.mt/x", "Export/Song OG/patterns/pattern_01.mtp.bak", "Export/Song OG/patterns/patternsMetadata.bak", "Export/Song OG/instruments/01 Kick.pti.bak"}) {
    INFO(none);
    CHECK(sv::classify(none) == Place::None);
  }
}

TEST_CASE("save guard OS-FILES-C T7: in an OG export folder a folder is made, a Temp is created and a Target appears by one rename in its own folder; nothing is replaced, moved or removed") {
  const std::string base = "Export/Song OG";
  // folders
  CHECK(sv::may_make_dir("Export"));
  CHECK(sv::may_make_dir((base).c_str()));
  CHECK(sv::may_make_dir((base + "/patterns").c_str()));
  CHECK(sv::may_make_dir((base + "/instruments").c_str()));
  for (const char* no : {"/sub", "/patterns/sub", "/instruments/sub", "/samples", "/project.mt", "/patterns/pattern_01.mtp", "/patterns/patternsMetadata", "/instruments/01 Kick.pti", "/patterns/_o3save.tmp"}) {
    INFO(no);
    CHECK_FALSE(sv::may_make_dir((base + no).c_str()));
  }
  CHECK_FALSE(sv::may_make_dir("Export/Song OG 100/patterns"));
  CHECK_FALSE(sv::may_make_dir("Export/Song/patterns"));
  CHECK_FALSE(sv::may_make_dir("Export/Song OG 1/patterns"));
  // temps
  CHECK(sv::may_create((base + "/_o3save.tmp").c_str()));
  CHECK(sv::may_create((base + "/patterns/_o3save.tmp").c_str()));
  CHECK(sv::may_create((base + "/instruments/_o3save.tmp").c_str()));
  CHECK_FALSE(sv::may_create((base + "/project.mt").c_str()));
  CHECK_FALSE(sv::may_create((base + "/patterns/pattern_01.mtp").c_str()));
  CHECK_FALSE(sv::may_create((base + "/patterns/patternsMetadata").c_str()));
  CHECK_FALSE(sv::may_create((base + "/instruments/01 Kick.pti").c_str()));
  CHECK_FALSE(sv::may_create((base + "/sub/_o3save.tmp").c_str()));
  CHECK_FALSE(sv::may_create((base + "/patterns/sub/_o3save.tmp").c_str()));
  CHECK_FALSE(sv::may_create("Export/Song/patterns/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("Export/Song OG 1/patterns/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("Projects/Song/patterns/_o3save.tmp"));
  // the three renames that finish a file
  CHECK(sv::may_rename((base + "/_o3save.tmp").c_str(), (base + "/project.mt").c_str()));
  CHECK(sv::may_rename((base + "/patterns/_o3save.tmp").c_str(), (base + "/patterns/pattern_01.mtp").c_str()));
  CHECK(sv::may_rename((base + "/patterns/_o3save.tmp").c_str(), (base + "/patterns/pattern_255.mtp").c_str()));
  CHECK(sv::may_rename((base + "/patterns/_o3save.tmp").c_str(), (base + "/patterns/patternsMetadata").c_str()));
  CHECK(sv::may_rename((base + "/instruments/_o3save.tmp").c_str(), (base + "/instruments/01 Kick.pti").c_str()));
  // and nothing else: not across folders, not into the user's, not a Target moved or backed up, not a Temp over a Temp
  const std::string tmp = base + "/_o3save.tmp";
  const std::string ptmp = base + "/patterns/_o3save.tmp";
  const std::string itmp = base + "/instruments/_o3save.tmp";
  CHECK_FALSE(sv::may_rename(tmp.c_str(), (base + "/patterns/pattern_01.mtp").c_str()));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), (base + "/project.mt").c_str()));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), (base + "/instruments/01 Kick.pti").c_str()));
  CHECK_FALSE(sv::may_rename(itmp.c_str(), (base + "/patterns/pattern_01.mtp").c_str()));
  CHECK_FALSE(sv::may_rename(itmp.c_str(), (base + "/patterns/patternsMetadata").c_str()));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), "Export/Song OG 2/patterns/pattern_01.mtp"));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), "Export/Song OG 2/project.mt"));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), "Projects/Song/project.mt"));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), "Projects/Song/patterns/pattern_01.mtp"));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), "Export/Song/project.mt"));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), "Export/project.mt"));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), (base + "/project.mt.bak").c_str()));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), (base + "/patterns").c_str()));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), (base + "/patterns/pattern_256.mtp").c_str()));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), (base + "/patterns/pattern_1.mtp").c_str()));
  CHECK_FALSE(sv::may_rename(ptmp.c_str(), (base + "/patterns/pattern_01.mtp.bak").c_str()));
  CHECK_FALSE(sv::may_rename(itmp.c_str(), (base + "/instruments/49 Kick.pti").c_str()));
  CHECK_FALSE(sv::may_rename(itmp.c_str(), (base + "/instruments/Kick.pti").c_str()));
  CHECK_FALSE(sv::may_rename(tmp.c_str(), ptmp.c_str()));
  CHECK_FALSE(sv::may_rename((base + "/project.mt").c_str(), (base + "/project.mt.bak").c_str()));
  CHECK_FALSE(sv::may_rename((base + "/project.mt").c_str(), "Export/Song OG 2/project.mt"));
  CHECK_FALSE(sv::may_rename((base + "/project.mt").c_str(), (base + "/patterns/project.mt").c_str()));
  CHECK_FALSE(sv::may_rename((base + "/patterns/pattern_01.mtp").c_str(), (base + "/patterns/pattern_02.mtp").c_str()));
  CHECK_FALSE(sv::may_rename((base + "/patterns/pattern_01.mtp").c_str(), (base + "/patterns/pattern_01.mtp.bak").c_str()));
  CHECK_FALSE(sv::may_rename((base + "/patterns/patternsMetadata").c_str(), (base + "/patterns/patternsMetadata.bak").c_str()));
  CHECK_FALSE(sv::may_rename((base + "/instruments/01 Kick.pti").c_str(), (base + "/instruments/02 Kick.pti").c_str()));
  CHECK_FALSE(sv::may_rename((base + "/patterns/pattern_01.mtp.bak").c_str(), (base + "/patterns/pattern_01.mtp").c_str()));
  CHECK_FALSE(sv::may_rename(base.c_str(), "Export/Song OG 2"));  // a folder is never renamed
  CHECK_FALSE(sv::may_rename((base + "/patterns").c_str(), (base + "/instruments").c_str()));
  // no removal but a Temp
  CHECK(sv::may_remove(tmp.c_str()));
  CHECK(sv::may_remove(ptmp.c_str()));
  CHECK(sv::may_remove(itmp.c_str()));
  for (const char* no : {"", "/project.mt", "/patterns", "/instruments", "/patterns/pattern_01.mtp", "/patterns/patternsMetadata", "/instruments/01 Kick.pti"}) {
    INFO(no);
    CHECK_FALSE(sv::may_remove((base + no).c_str()));
  }
  CHECK_FALSE(sv::may_remove("Export"));
}

TEST_CASE("save guard OS-FILES-C T7: the older places are exactly what they were (the OG rows add to Export/<name>/ and nothing else)") {
  CHECK(sv::classify("Export/Song/Song_Song0001.wav") == Place::Target);
  CHECK(sv::classify("Export/Song/_o3save.tmp") == Place::Temp);
  CHECK(sv::classify("Export/Song/project.mt") == Place::None);
  CHECK(sv::classify("Export/Song/patterns") == Place::None);
  CHECK(sv::classify("Export/Song/patterns/pattern_01.mtp") == Place::None);
  CHECK(sv::classify("Export/Song/sub/a.wav") == Place::None);
  CHECK(sv::classify("Projects/Song/project.mt") == Place::None);
  CHECK(sv::classify("Projects/Song/patterns/pattern_01.mtp") == Place::None);
  CHECK(sv::classify("Projects/Song/project.o3pj") == Place::Target);
  CHECK(sv::classify("instruments/01 Kick.pti") == Place::Target);
  CHECK(sv::classify("Export/Song OG/Song OG_Song0001.wav") == Place::Target);  // (a stems folder of a project that is called "Song OG": the guard cannot tell it by shape; it is a WAV there)
  static_assert(sv::og_export_folder("A OG 99"), "constexpr");
  static_assert(!sv::og_export_folder("A OG 100"), "constexpr");
  static_assert(sv::classify("Export/A OG/project.mt") == Place::Target, "constexpr");
}

TEST_CASE("save guard OS-SCREENSHOT: Screenshots/SHOT_nnnn.bmp is a Target made by the rename of the Temp, and nothing else is") {
  CHECK(sv::classify("Screenshots") == Place::Folder);
  CHECK(sv::classify("screenshots") == Place::Folder);
  CHECK(sv::may_make_dir("Screenshots"));
  CHECK(sv::classify("Screenshots/_o3save.tmp") == Place::Temp);
  CHECK(sv::may_create("Screenshots/_o3save.tmp"));
  CHECK(sv::classify("Screenshots/SHOT_0001.bmp") == Place::Target);
  CHECK(sv::classify("SCREENSHOTS/shot_9999.BMP") == Place::Target);
  CHECK(sv::may_rename("Screenshots/_o3save.tmp", "Screenshots/SHOT_0042.bmp"));
  // the near misses: number 0 or 5 digits or 3, another prefix or suffix, a Backup form, a deeper path, another folder
  for (const char* bad : {"Screenshots/SHOT_0000.bmp", "Screenshots/SHOT_00001.bmp", "Screenshots/SHOT_001.bmp", "Screenshots/SHOT_00a1.bmp", "Screenshots/SHOT_0001.png", "Screenshots/SHOT_0001.bmp.bak",
                          "Screenshots/shot0001.bmp", "Screenshots/OTHER_0001.bmp", "Screenshots/x/SHOT_0001.bmp", "Screenshots/../SHOT_0001.bmp", "SHOT_0001.bmp", "Screens/SHOT_0001.bmp"}) {
    INFO(bad);
    CHECK(sv::classify(bad) == Place::None);
    CHECK_FALSE(sv::may_create(bad));
    CHECK_FALSE(sv::may_remove(bad));
  }
  CHECK_FALSE(sv::may_remove("Screenshots/SHOT_0001.bmp"));       // a finished screenshot is never removed by the layer
  CHECK_FALSE(sv::may_create("Screenshots/SHOT_0001.bmp"));       // nothing is written to a Target directly
  CHECK_FALSE(sv::may_rename("Screenshots/SHOT_0001.bmp", "Screenshots/SHOT_0002.bmp"));  // no Target -> Target
  CHECK_FALSE(sv::may_rename("Screenshots/_o3save.tmp", "Samples/SHOT_0001.bmp"));        // one folder
  CHECK_FALSE(sv::may_rename("Screenshots/_o3save.tmp", "Screenshots/SHOT_0001.wav"));    // a Temp becomes a screenshot only under a screenshot's name
}

TEST_CASE("save guard OS-MODPRESET P3: modules/presets/<stem>/<name>.o3mp, a never-replaced Target below a folder chain, nothing else under modules/") {
  for (const char* folder : {"modules", "MODULES", "modules/presets", "Modules/Presets", "modules/presets/tb303", "modules/presets/TB 303"}) {
    CHECK(sv::classify(folder) == Place::Folder);
    CHECK(sv::may_make_dir(folder));
  }
  CHECK(sv::classify("modules/presets/tb303/Acid Bass.o3mp") == Place::Target);
  CHECK(sv::classify("modules/presets/tb303/ACID 2.O3MP") == Place::Target);
  CHECK(sv::classify("modules/presets/tb303/_o3save.tmp") == Place::Temp);
  CHECK(sv::may_create("modules/presets/tb303/_o3save.tmp"));
  CHECK(sv::may_remove("modules/presets/tb303/_o3save.tmp"));
  CHECK(sv::may_rename("modules/presets/tb303/_o3save.tmp", "modules/presets/tb303/Acid.o3mp"));
  // The longest path: a 31-byte stem and a 30-byte name; a 31-byte name is refused (the rule of tools/o3preset.py valid_file_name).
  const std::string stem(sv::kNameMax, 's');
  const std::string longest = "modules/presets/" + stem + "/" + std::string(sv::kPresetNameMax, 'n') + ".o3mp";
  CHECK(longest.size() + 1 <= sv::kPathCap);
  CHECK(sv::classify(longest.c_str()) == Place::Target);
  CHECK(sv::classify(("modules/presets/" + stem + "/" + std::string(sv::kPresetNameMax + 1, 'n') + ".o3mp").c_str()) == Place::None);
  CHECK(sv::classify(("modules/presets/" + stem + "x").c_str()) == Place::None);
  // The names tools/o3preset.py refuses are refused here too (the two lists are held equal: the same names are in o3preset_test.py).
  for (const char* bad : {"", " lead", "trail ", "trail.", ".hidden", "a..b", "CON", "nul.x", "COM1", "lpt9", "a/b", "a\\b", "a:b", "caf\xC3\xA9", "tab\tx"}) {
    CHECK(sv::classify((std::string("modules/presets/tb303/") + bad + ".o3mp").c_str()) == Place::None);
  }
  CHECK(sv::classify("modules/presets/tb303/.o3mp") == Place::None);
  // Nothing else under modules/, no Backup form, no fifth component, no traversal, no other extension.
  for (const char* none : {"modules/instruments", "modules/instruments/tb303.tmod", "modules/tb303.tmod", "modules/presets/tb303/Acid.o3mp.bak",
                           "modules/presets/tb303/Acid.o3mp.tmp", "modules/presets/tb303/Acid.wav", "modules/presets/tb303/Acid.pti", "modules/presets/tb303/Acid", "modules/presets/tb303/sub/Acid.o3mp",
                           "modules/presets/tb303/sub/_o3save.tmp", "modules/presets/..", "modules/presets/../x/Acid.o3mp", "modules/presets/tb303/../Acid.o3mp", "modules/../Projects/a/project.o3pj",
                           "modules/presets/_o3save.tmp", "modules/_o3save.tmp", "modules/presets/CON", "modules/presets/ x/Acid.o3mp", "modules/presets/tb303//Acid.o3mp", "modules//presets",
                           "modules/presets/tb303/", "modules/preset/tb303/Acid.o3mp", "modules/presets/tb303/Acid.o3mp/x"}) {
    CAPTURE(none);
    CHECK(sv::classify(none) == Place::None);
  }
  // A Target is never removed, renamed or replaced; a folder is never removed or renamed.
  CHECK_FALSE(sv::may_remove("modules/presets/tb303/Acid.o3mp"));
  CHECK_FALSE(sv::may_remove("modules/presets/tb303"));
  CHECK_FALSE(sv::may_rename("modules/presets/tb303/Acid.o3mp", "modules/presets/tb303/Acid.o3mp.bak"));
  CHECK_FALSE(sv::may_rename("modules/presets/tb303/Acid.o3mp", "modules/presets/tb303/Other.o3mp"));
  CHECK_FALSE(sv::may_rename("modules/presets/tb303/_o3save.tmp", "modules/presets/sid/Acid.o3mp"));
  CHECK_FALSE(sv::may_rename("modules/presets/tb303/_o3save.tmp", "modules/tb303.tmod"));
  CHECK_FALSE(sv::may_rename("modules", "modules2"));
  CHECK_FALSE(sv::may_make_dir("modules/presets/tb303/Acid.o3mp"));
  CHECK_FALSE(sv::may_make_dir("modules/instruments"));
}
