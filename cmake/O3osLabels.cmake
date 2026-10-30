# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every ctest has an AREA label (OS-CIFAST): which part of the tree its verdict is about. `tools/ci.sh` with O3OS_CI_SCOPE=changed maps
# the files of a change to areas and runs `ctest -L` on them (see the CI section of README.md); the full run (the default) ignores labels.
#
#   core model formats seq audio app platform python fuzz    the areas of the scope map (tools/ci.sh)
#   manual                                                   the manual's generated pictures (docs/manual/img/): a changed PNG or SVG, or its manifest, is not "docs only"
#   qanet                                                    OS-QANET: the test net of docs/specs/OS-QANET.md (tests/qanet/); never alone, it carries an area label too
#   static                                                   repo-wide structure checks (layers, wiring, unit coverage): cheap, run in every scope
#   e2e                                                      OS-E2E: the end-to-end battery, on demand and at releases (tools/ci.sh e2e). Never alone: a test carries an area label too,
#                                                            and tools/ci.sh leaves `e2e` out of every other run (ctest -LE '^e2e$'); it is not in ALL_LABELS_RE
#
#   slow                                                     OS-CISPEED: a test that takes over a minute (the QANET self-tests and the fixed-seed random user, the manual's image check). Never alone: a test
#                                                            carries an area label too. `tools/ci.sh` leaves it out of a scoped run (O3OS_CI_SCOPE=changed) and keeps it in the full run
#   long                                                     OS-CISPEED: the e2e tests that take 5 minutes or more each (the random user's 8 shards). Always with `e2e`; `tools/ci.sh e2e` runs
#                                                            them only with O3OS_CI_LONG=1 (the orchestrator's batch run and the image gate)
#
# A test may have several (a Python reference of a format is `formats` and `python`). o3os_label_tests() runs at the end of each directory
# that adds tests (add_test and set_tests_properties belong to the directory) and gives every test of it the labels of the first rule that
# its name matches; a test that matches none stops the configure, so a new test cannot be left out of every scope. Labels a test has already
# (qemu, tsan) stay.
#
# A rule is a pair: a regular expression of the test name, and the labels, separated by commas. Order matters: the first match wins.
set(O3OS_LABEL_RULES
  "^(layers|layers_selftest|wiring_audit|wiring_audit_selftest|unit_cover)$" "static"
  "^(keytext|keytext_selftest)$" "static,app"  # OS-KEYROWDLG S4: the key names of app/ against the key tables
  "^hil_scenarios_sync$" "static,platform"  # OS-CLEANUP: the HIL scenarios of platform/mk66/tools/hil/ against what gen_hil.py writes (tests/hil_scenarios_sync_test.py)
  "^sim_project_drift$" "static,platform"  # OS-E2E-1b: SimProject against platform/mk66/src/app_main.cpp's load sequence (tests/sim_project_drift_test.py)
  "^qa_cover$" "static"                 # OS-E2E-3: the QA catalogue gate (tools/qa_cover.py --check); a catalogue edit maps to this label
  "^qa_cover_selftest$" "static,python" # its self-test: python, and static so that a change of the tool alone (scope map: static) runs it
  "^code_map$" "static,app"             # OS-CODEMAP: the agents' map of the tree (tools/code_map.py --check): every binding resolves to a handler
  "^scenario_fatfs_" "app,platform,e2e"  # OS-QANET D1: the card journeys of sim/scenarios/e2e/ again on a FAT32 image through the unit's FatFs; on demand like the journeys (tools/ci.sh e2e)
  "^qanet_mutants_selftest$" "static,python,qanet"  # OS-QANET F1: every patch of tests/qanet/mutants/ has its header, the tool parses and tabulates (no build); the run itself (tools/qanet_mutants.py --run) is on demand
  "^qanet_ledger$" "static,qanet"       # OS-QANET A4: the ledger of the bugs a person found (docs/qa/qanet-ledger.md, tools/qanet_ledger.py --check); a ledger edit maps to this label (tools/ci.sh: docs/qa/*)
  "^qanet_ledger_selftest$" "static,python,qanet"  # its self-test: python, and static so that a change of the tool alone runs it
  "^qanet_audio_matrix" "audio,app,qanet"  # OS-QANET C1 to C3: pad against Pattern step, one ctest a cell and one for the self-tests (tests/qanet/audio_matrix_test.cpp); `qanet` selects it with ctest -L
  "^qanet_audio_probe_selftest$" "audio,app,qanet,slow"  # OS-CISPEED: 40 s
  "^qanet_audio_probe" "audio,app,qanet"  # OS-QANET C4: the probe's self-test (tests/qanet/audio_probe.cpp, random_user.cpp --audio-probe-selftest); before the `^qanet_` catch-all
  "^qanet_random_user_fatfs_[0-9]+of[0-9]+$" "app,platform,qanet,e2e"  # OS-QANET D1: the random user on the FAT32 image, the 200 seeds in shards; on demand like the folder's
  "^qanet_random_user_fatfs_short$" "app,platform,qanet"  # OS-QANET D1: seeds 1..2 on the image (ci.sh host)
  "^qanet_random_user_[0-9]+of[0-9]+$" "app,qanet,e2e,long"  # OS-QANET B1: the 200 seeds of the random user, in shards; on demand (tools/ci.sh e2e, tools/remote_ci.sh e2e); the seed range and steps come from the environment
  "^qanet_(random_rules_selftest|random_rules23_selftest|random_user_short|shrink_selftest)$" "app,qanet,slow"  # OS-CISPEED: 177 s, 74 s, 67 s, 58 s under the sanitizers; a full run keeps them
  "^qanet_" "app,qanet"                 # OS-QANET: the rule tests over every bound action (tests/qanet/); `qanet` selects them with ctest -L, `app` is their scope
  "^unit_fuzz$" "fuzz"
  "^unit_model$" "model"
  "^unit_formats$" "formats"
  "^unit_usb$" "platform"
  "^unit_audio$" "audio"
  "^unit_seq$" "seq"
  "^unit_(plot|ui|apps)$" "app"
  "^unit_rest$" "core,app"
  "^(contrast|budget|translate_fallback|boot_logo|plot|plot_budget|plot_bench_smoke)$" "app"
  "^audio_bench_smoke$" "audio"
  "^seq_bench_smoke$" "seq"
  "^mk66_" "platform"
  "^o3sim_manual_images$" "app,manual,slow"  # OS-MANUALIMG: the manual's screenshots against a fresh render
  "^manual_diagrams$" "static,manual"   # OS-MANUALDIAG: the manual's SVG diagrams against the bindings, the boot chords and the audio graph
  "^legibility_" "app,e2e"              # OS-E2E-5: the legibility sweep, sharded like the gallery; on demand (tools/ci.sh e2e), not in `ci.sh host` nor on GitHub
  "^scenario_e2e_" "app,e2e"            # OS-E2E-1a: the journeys of sim/scenarios/e2e/ (assertions, a card): on demand, like the legibility sweep
  "^e2e_ref_" "app,audio,formats,seq,python,e2e"  # OS-E2E-9: the reference players (libopenmpt, scipy, Open303) inside the journeys; on demand like the journeys
  "^(scenario_|gallery_|o3sim_)" "app"
  "^update_goldens_selftest$" "app,python"
  "^fingerprint_guard$" "static"        # OS-E2E-4: git against the fingerprint files (tools/check_fingerprint_guard.py): every scope, it reads no build
  "^(fingerprint_guard_selftest|update_audio_fingerprints_selftest)$" "static,python"  # OS-E2E-4: the two tools' self-tests
  "^(font|icon|logo)_convert" "app"
  "^e2e_((card|pro)_)?fixture_fresh$" "app,formats,python"  # OS-E2E-1b, OS-E2E-13, J2: the generated cards of the journeys (tests/e2e_card_fixture_test.py, e2e_pro_fixture_test.py, e2e_og_fixture_test.py): a sim, format or python change runs them
  "^(project_python|heavy_song_python|hil_songs_python|med_python|med_import_python|med_openmpt|mod_python|modimp_openmpt)$" "formats,python"
  "^corefx_openmpt$" "formats,seq,audio,python"  # OS-COREFX: importer -> sequencer -> engine against libopenmpt
  "^(med_python_selftest|mod_python_selftest|manual_tools_selftest|manual_tables_selftest|starter_python|testdata_python|e2e_report_selftest|qa_sheet_python_selftest)$" "python"
  "^o3m_.*_test$" "audio"
  "^o3m_" "python"
  "^ring_tsan$" "core"
  "^usb_ring_tsan$" "platform"
  "^(audio_status_tsan|audio_taps_tsan|module_perf_tsan|audio_offline_tsan)$" "audio"
  "^song_order_tsan$" "seq,model"  # E-SONG-07: the order list's edit journal (Project::snapshot_order) against a writer thread
  # The QEMU suites (tests/CMakeLists.txt o3os_target_suite): by the library that the suite tests.
  "^qemu_(core|jobs)$" "core"
  "^qemu_(wav|wav_sample|wavetable_import|iff_sample|formats_.*)$" "formats"
  "^qemu_(model_undo|params|params_model|pattern_rules|dir_index)$" "model"
  "^qemu_(usb|usb_device|usb_return|mk66|platform|probe|probe_stack)$" "platform"
  "^qemu_(audio.*|module_.*)$" "audio"
  "^qemu_(midi|seq|seq_model|metro_sync)$" "seq"
  "^qemu_(eve|plot|boot)$" "app"
)

function(o3os_label_tests)
  get_property(_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
  list(LENGTH O3OS_LABEL_RULES _rules)
  foreach(_test IN LISTS _tests)
    set(_labels "")
    set(_i 0)
    while(_i LESS _rules)
      list(GET O3OS_LABEL_RULES ${_i} _regex)
      math(EXPR _j "${_i} + 1")
      list(GET O3OS_LABEL_RULES ${_j} _rule_labels)
      if("${_test}" MATCHES "${_regex}")
        string(REPLACE "," ";" _labels "${_rule_labels}")
        break()
      endif()
      math(EXPR _i "${_i} + 2")
    endwhile()
    if("${_labels}" STREQUAL "")
      message(FATAL_ERROR "ctest '${_test}' (${CMAKE_CURRENT_SOURCE_DIR}) has no area label: add a rule to cmake/O3osLabels.cmake")
    endif()
    set_property(TEST ${_test} APPEND PROPERTY LABELS ${_labels})
  endforeach()
endfunction()
