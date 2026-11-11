# SPDX-License-Identifier: GPL-3.0-or-later
# app: the application (the screens, the overlays, static demo data) and its input pipeline's app side: the action
# registry and its feature files, the built-in bindings. Depends on ui (and, through it, core), on the model for undo and on
# the WAV importer for the first job (OS-002c). og_folder (OS-007e) loads an OG project folder from a hal::storage::Storage with the OG readers; og_samples
# (OS-007f) then loads its instruments' PCM into the sample arena and binds it to the audio layer's voice table (hence the dependency on audio).
# sample_bank (OS-011a) is the sample memory the sample screens use (per-slot PCM, the arena, the safe handover to the voices); sample_actions and
# screens/sample_load_screen.cpp are the Sample load screen on the File screen's browser.
# dir_index / dir_scan (OS-010a) are the folder browser's core (a sorted listing, and the job that fills it from a hal::storage::Storage); file_view,
# file_actions and screens/file_screen.cpp are the File screen on it.
# OS-FLASHDIET A6/A7: the help pages packed into one blob. tools/help_pack.py packs app/help/pages.txt (the
# authoring file, made once from the old table in src/help_text.cpp, which A7 removed) with tools/deflate_pack.py
# into a generated help_blob.cpp in the build tree: the deflate stream, the page index and the page titles.
# src/help_blob.cpp inflates a page out of it, and src/help_text.cpp's help_page() serves its pages from the
# 4 KiB page buffer (A7: the blob is live in the image).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_help_blob_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/help_blob.cpp")
add_custom_command(
  OUTPUT "${_help_blob_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/help_pack.py"
          --pages "${CMAKE_CURRENT_SOURCE_DIR}/help/pages.txt" --out "${_help_blob_gen}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/help/pages.txt"
          "${PROJECT_SOURCE_DIR}/tools/help_pack.py" "${PROJECT_SOURCE_DIR}/tools/deflate_pack.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/app/help_blob.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/app/help_text.h"
  COMMENT "Packing the help pages (tools/help_pack.py)"
  VERBATIM)

# OS-FLASHDIET P3: the FX picker's names and descriptions (app/fx/fx_text.txt) packed the same way into a generated fx_text_blob.cpp (tools/fx_text_pack.py);
# src/fx_text.cpp reads a line out of it through the shared decoder.
set(_fx_blob_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/fx_text_blob.cpp")
add_custom_command(
  OUTPUT "${_fx_blob_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/fx_text_pack.py"
          --text "${CMAKE_CURRENT_SOURCE_DIR}/fx/fx_text.txt" --out "${_fx_blob_gen}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fx/fx_text.txt"
          "${PROJECT_SOURCE_DIR}/tools/fx_text_pack.py" "${PROJECT_SOURCE_DIR}/tools/deflate_pack.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/app/fx_text.h"
          "${PROJECT_SOURCE_DIR}/model/include/o3os/model/fx_table.h"
  COMMENT "Packing the FX picker text (tools/fx_text_pack.py)"
  VERBATIM)

# OS-FLASHDIET P6: the built-in key bindings table (src/bindings.cpp, 10.9 KB of constexpr data) deflated into a generated builtin_bindings_blob.cpp. The bytes come
# from the cross compiler itself, so the layout is the target's: bindings.cpp is compiled a second time with O3OS_BINDINGS_DUMP into the object library
# app_bindings_dump (never linked; same flags as app, no sanitizer so a redzone cannot pad the dump), objcopy lifts the dump section out of that object,
# tools/bindings_pack.py (deterministic encoder, not zlib) packs it. The same sources and tools give the same blob, so the reproducible image gate holds.
add_library(app_bindings_dump OBJECT src/bindings.cpp)
target_include_directories(app_bindings_dump PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(app_bindings_dump PRIVATE ui model formats formats_wav audio midi module seq)
target_compile_definitions(app_bindings_dump PRIVATE O3OS_BINDINGS_DUMP=1)
o3os_strict_warnings(app_bindings_dump)
target_compile_options(app_bindings_dump PRIVATE -fsingle-precision-constant)
if(O3OS_SANITIZE OR O3OS_TSAN)
  target_compile_options(app_bindings_dump PRIVATE -fno-sanitize=all)
endif()
set(_bindings_bin "${CMAKE_CURRENT_BINARY_DIR}/gen/builtin_bindings.bin")
set(_bindings_blob_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/builtin_bindings_blob.cpp")
add_custom_command(
  OUTPUT "${_bindings_blob_gen}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/gen"
  COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.rodata.o3_builtin_bindings $<TARGET_OBJECTS:app_bindings_dump> "${_bindings_bin}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/bindings_pack.py" --bin "${_bindings_bin}" --out "${_bindings_blob_gen}"
  DEPENDS app_bindings_dump $<TARGET_OBJECTS:app_bindings_dump>
          "${PROJECT_SOURCE_DIR}/tools/bindings_pack.py" "${PROJECT_SOURCE_DIR}/tools/deflate_pack.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/app/bindings.h"
  COMMAND_EXPAND_LISTS
  COMMENT "Packing the built-in bindings (tools/bindings_pack.py)"
  VERBATIM)

o3os_library(app
  SOURCES src/app.cpp src/text_format.cpp src/fx_text.cpp "${_fx_blob_gen}" "${_bindings_blob_gen}" src/pattern_data.cpp src/pattern_view.cpp src/song_data.cpp src/song_view.cpp src/song_actions.cpp src/song_map.cpp src/song_notemap.cpp src/song_stage.cpp src/song_timeline.cpp src/screens/song_pages.cpp src/song_edit.cpp src/song_build.cpp src/song_map_actions.cpp src/song_tools_view.cpp src/screens/song_tools_overlay.cpp
          src/actions.cpp src/nav_actions.cpp src/screen_actions.cpp src/pattern_actions.cpp src/pad_audition_actions.cpp src/ui_actions.cpp
          src/edit_actions.cpp src/job_actions.cpp src/param_actions.cpp src/cc_map.cpp src/import_job.cpp src/peak_job.cpp src/og_folder.cpp src/wt_user.cpp src/native_load.cpp src/og_samples.cpp src/module_import.cpp src/module_binder.cpp src/module_ui.cpp
          src/dir_index.cpp src/dir_scan.cpp src/file_view.cpp src/file_actions.cpp src/file_ops_actions.cpp src/recent_list.cpp
          src/sample_bank.cpp src/sample_actions.cpp src/wave_math.cpp src/transient.cpp src/slice_job.cpp src/preview_job.cpp src/pti_load.cpp src/og_pcm.cpp src/audition.cpp src/rec_actions.cpp src/lane_record.cpp src/text_entry.cpp src/text_actions.cpp src/sample_dsp.cpp src/amiga_convert.cpp src/sample_edit_job.cpp src/ptfx_job.cpp src/ptfx_page.cpp
          src/bindings.cpp src/chrome.cpp src/pad_board.cpp src/step_actions.cpp src/pattern_edit_util.cpp src/pattern_ops_actions.cpp src/fill_view.cpp src/fill_actions.cpp src/screens/fill_overlay.cpp src/chooser_actions.cpp src/pattern_tools_view.cpp src/pattern_tools_actions.cpp src/screens/pattern_tools_overlay.cpp
          src/screens/registry.cpp src/screens/pattern_screen.cpp src/screens/song_screen.cpp src/screens/params_screen.cpp src/screens/overlays.cpp
          src/key_prompt.cpp src/help_text.cpp src/help_blob.cpp "${_help_blob_gen}" src/help_actions.cpp src/screens/help_overlay.cpp src/screens/dialog.cpp src/screens/scene.cpp src/input_test.cpp src/screens/input_test_screen.cpp src/screens/file_screen.cpp src/screens/sample_load_screen.cpp src/screens/wave_screen.cpp src/wave_actions.cpp src/screens/sample_edit_screen.cpp src/sample_edit_actions.cpp src/screens/config_screen.cpp src/mix_board.cpp src/project_state.cpp src/mixer_actions.cpp src/screens/mixer_screen.cpp src/perform_actions.cpp src/dj_actions.cpp src/screens/perform_screen.cpp src/screens/modules_screen.cpp src/screens/module_picker.cpp src/screens/play_mode_overlay.cpp src/screens/preset_overlay.cpp src/preset_list.cpp src/preset_actions.cpp src/screens/text_entry_overlay.cpp src/module_actions.cpp src/tools_actions.cpp src/verb_actions.cpp src/screens/verb_menu_overlay.cpp src/instrument_actions.cpp src/events_actions.cpp src/screens/events_overlay.cpp src/learn_map_actions.cpp src/screens/learn_map_overlay.cpp
          src/recorder.cpp src/take_overview.cpp src/record_actions.cpp src/screens/record_screen.cpp
          src/atomic_save.cpp src/og_export.cpp src/og_timing.cpp src/app_export.cpp src/save_service.cpp src/save_actions.cpp src/export_source.cpp src/export_stems.cpp
          src/export_actions.cpp src/screens/export_overlay.cpp src/firmware_actions.cpp src/screens/firmware_overlay.cpp
          src/step_popup_actions.cpp src/screens/step_popup_overlay.cpp src/preview_actions.cpp src/pad_lights.cpp src/pad_mode_actions.cpp src/state_line.cpp
  DEPENDS ui model formats formats_wav audio midi module seq)
# OS-SRAMU: on the MK66 image the plots' working memory (4.5 KB, main loop only) is the platform's, in the SDRAM slack window (params_screen.cpp).
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(app PRIVATE O3OS_CURVE_SCRATCH_EXTERNAL=1)
endif()
# OS-FLASHDIET A7: on the MK66 image the help page buffer (HelpPageBuf, 4 KiB, main loop only) is the platform's, in the SDRAM slack window
# (platform/mk66/src/app_main.cpp defines help_page_buf()); this drops app's own copy in help_text.cpp. Host, the simulator and QEMU keep app's own.
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(app PRIVATE O3OS_HELP_PAGE_BUF_EXTERNAL=1)
endif()
# OS-FLASHDIET P3: likewise the FX picker's decoded-text cache (FxTextCache, 0.9 KB, main loop only) is the platform's, in SDRAM (app_main.cpp defines fx_text_cache()).
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(app PRIVATE O3OS_FX_TEXT_CACHE_EXTERNAL=1)
endif()
# OS-FLASHDIET P6: on the MK66 image the constexpr built-in bindings table is not linked in; the platform unpacks the blob into SDRAM at boot (start_app in app_main.cpp).
# Host, the simulator and QEMU keep the constexpr table (and the host test holds the blob to it).
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(app PRIVATE O3OS_BUILTIN_BINDINGS_PACKED=1)
endif()
