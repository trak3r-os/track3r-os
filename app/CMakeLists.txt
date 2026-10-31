# SPDX-License-Identifier: GPL-3.0-or-later
# app: the application (the screens, the overlays, static demo data) and its input pipeline's app side: the action
# registry and its feature files, the built-in bindings. Depends on ui (and, through it, core), on the model for undo and on
# the WAV importer for the first job (OS-002c). og_folder (OS-007e) loads an OG project folder from a hal::storage::Storage with the OG readers; og_samples
# (OS-007f) then loads its instruments' PCM into the sample arena and binds it to the audio layer's voice table (hence the dependency on audio).
# sample_bank (OS-011a) is the sample memory the sample screens use (per-slot PCM, the arena, the safe handover to the voices); sample_actions and
# screens/sample_load_screen.cpp are the Sample load screen on the File screen's browser.
# dir_index / dir_scan (OS-010a) are the folder browser's core (a sorted listing, and the job that fills it from a hal::storage::Storage); file_view,
# file_actions and screens/file_screen.cpp are the File screen on it.
o3os_library(app
  SOURCES src/app.cpp src/pattern_data.cpp src/pattern_view.cpp src/song_data.cpp src/song_view.cpp src/song_actions.cpp src/song_map.cpp src/song_notemap.cpp src/song_stage.cpp src/song_timeline.cpp src/screens/song_pages.cpp src/song_edit.cpp src/song_map_actions.cpp src/song_tools_view.cpp src/screens/song_tools_overlay.cpp
          src/actions.cpp src/nav_actions.cpp src/screen_actions.cpp src/pattern_actions.cpp src/pad_audition_actions.cpp src/ui_actions.cpp
          src/edit_actions.cpp src/job_actions.cpp src/param_actions.cpp src/import_job.cpp src/peak_job.cpp src/og_folder.cpp src/native_load.cpp src/og_samples.cpp src/module_import.cpp src/module_binder.cpp src/module_ui.cpp
          src/dir_index.cpp src/dir_scan.cpp src/file_view.cpp src/file_actions.cpp src/file_ops_actions.cpp src/recent_list.cpp
          src/sample_bank.cpp src/sample_actions.cpp src/wave_math.cpp src/transient.cpp src/slice_job.cpp src/preview_job.cpp src/pti_load.cpp src/og_pcm.cpp src/audition.cpp src/rec_actions.cpp src/text_entry.cpp src/text_actions.cpp src/sample_dsp.cpp src/amiga_convert.cpp src/sample_edit_job.cpp
          src/bindings.cpp src/chrome.cpp src/pad_board.cpp src/step_actions.cpp src/pattern_ops_actions.cpp src/fill_view.cpp src/fill_actions.cpp src/screens/fill_overlay.cpp src/chooser_actions.cpp
          src/screens/registry.cpp src/screens/pattern_screen.cpp src/screens/song_screen.cpp src/screens/params_screen.cpp src/screens/overlays.cpp
          src/key_prompt.cpp src/help_text.cpp src/help_actions.cpp src/screens/help_overlay.cpp src/screens/dialog.cpp src/screens/scene.cpp src/input_test.cpp src/screens/input_test_screen.cpp src/screens/file_screen.cpp src/screens/sample_load_screen.cpp src/screens/wave_screen.cpp src/wave_actions.cpp src/screens/sample_edit_screen.cpp src/sample_edit_actions.cpp src/screens/config_screen.cpp src/mix_board.cpp src/project_state.cpp src/mixer_actions.cpp src/screens/mixer_screen.cpp src/perform_actions.cpp src/dj_actions.cpp src/screens/perform_screen.cpp src/screens/modules_screen.cpp src/screens/module_picker.cpp src/screens/play_mode_overlay.cpp src/screens/text_entry_overlay.cpp src/module_actions.cpp src/tools_actions.cpp src/screens/tools_overlay.cpp src/instrument_actions.cpp
          src/recorder.cpp src/take_overview.cpp src/record_actions.cpp src/screens/record_screen.cpp
          src/atomic_save.cpp src/og_export.cpp src/og_timing.cpp src/app_export.cpp src/save_service.cpp src/save_actions.cpp src/export_source.cpp src/export_stems.cpp
          src/export_actions.cpp src/screens/export_overlay.cpp src/firmware_actions.cpp src/screens/firmware_overlay.cpp
          src/step_popup_actions.cpp src/screens/step_popup_overlay.cpp src/preview_actions.cpp src/pad_lights.cpp src/pad_mode_actions.cpp src/state_line.cpp
  DEPENDS ui model formats formats_wav audio midi module seq)
# OS-SRAMU: on the MK66 image the plots' working memory (4.5 KB, main loop only) is the platform's, in the SDRAM slack window (params_screen.cpp).
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(app PRIVATE O3OS_CURVE_SCRATCH_EXTERNAL=1)
endif()
