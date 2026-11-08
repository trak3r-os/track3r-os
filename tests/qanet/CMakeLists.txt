# SPDX-License-Identifier: GPL-3.0-or-later
# OS-QANET B1 (docs/specs/OS-QANET.md part B): the random user. A standalone program (not doctest: it takes --seed, --seeds and --steps for the long runs) over the simulator and the large generated card
# (sim/CMakeLists.txt, target qanet_card_fixture). Tests, all of the area labels `app` + `qanet` (cmake/O3osLabels.cmake):
#   qanet_random_user_selftest   the generator is deterministic, a planted directory leak is found, the baseline's parser and comparison
#   qanet_shrink_selftest        B2: a planted fault is shrunk to under 15 commands, written as a scenario that fails under o3sim with the same message
#   qanet_assert_selftest        B2 fold: the assertions a written scenario ends with (listing, undo) pass on the state they want and fail on the one the bug leaves, under the real o3sim
#   qanet_random_rules_selftest         B4: a rule-1 breach planted after a sample slot is filled is found at that step (and not before), shrunk and written
#   qanet_random_rules23_selftest  B4b: a silent key that appears only after the document changed mid-walk (rule 2) and an out-of-span edit after a mid-walk selection (rule 3) are found at that step (and not before), shrunk and written
#   qanet_random_meta_selftest   B5: each metamorphic relation of --meta is reached, a save that loses a field is found, and its written scenario fails under o3sim
#   qanet_random_exhaust_selftest  B8: the limit fixtures (--exhaust) each reach their limit, find nothing in a clean run, and find a defect planted at it (rule 1, also with the sample-edit history full)
#   qanet_random_exhaust         B8: seeds 1..2 as qanet_random_user_short, with --exhaust --exhaust-all instead of --meta: the undo cap, the sample-edit history cap and the first edit after a slot switch (3 fixtures, 6 reached) on those two seeds only (the eight shards do not run --exhaust)
#   qanet_random_exhaust_pool    B8: the voice pool saturated by 13 to 16 held pads (seeds 1..2, on a simulator of their own: `document sound` with the engine), released in a random order: the pool never holds more than 12 voices, none stays after the release
#   qanet_random_exhaust_folder  B8: a folder of 2000 files (made at test time in a scratch directory) in Sample load: the listing holds 256 and says so, the cursor stops at both ends, a file of the last row loads, the full flag does not outlive the folder
#   qanet_random_exhaust_fresh_selftest  B8: the two fixtures above each reach their limit, find nothing in a clean run, and find a defect planted at it (a lost key-up; a listing that lost entries)
#   qanet_random_user_short      seeds 1..2, 300 steps: the fixed-seed run that is part of `ci.sh host` (about 30 s under the sanitizers); with the audio probe (C4: the walk has the engine, every rebind of an instrument's descriptor is replayed and probed)
#   qanet_audio_probe_selftest   C4: a scripted walk is probed mid-walk, a planted detune after an undo is found, and without it the settled pad and the Pattern step agree (the fresh-pad findings are the ones the baseline lists)
#   qanet_random_user_<i>of8     seeds 1..72, 300 steps in eight shards of 9 seeds (B4b: 40 s a seed with the rules at every input and the audio probe, 5 to 7.5 min a shard; the 200 seeds took 16-18 min; the older figure: about 17 s a seed under the sanitizers, and a replay and about four probes a seed more with the audio probe and its edit session) (label `e2e`: `tools/ci.sh e2e`, tools/remote_ci.sh e2e); the seed range and the step count come from the environment
#                                (O3OS_QANET_SEED, O3OS_QANET_SEEDS, O3OS_QANET_STEPS; tools/remote_ci.sh passes them on) for the long runs on the remote machine
# `tools/ci.sh e2e` runs the eight shards of 25 seeds each: about 1 h of CPU under the sanitizers (a local run holds a core for that long: run it on the remote machine, `tools/remote_ci.sh e2e`).
# The findings are compared with tests/qanet/baseline_random.txt; the run fails on a finding that is not in it and on a baseline line of a covered seed that no longer occurs.
get_property(_cards_dir GLOBAL PROPERTY O3SIM_CARDS_DIR)
add_executable(o3_qanet_random_user random_user.cpp audio_probe.cpp)
target_link_libraries(o3_qanet_random_user PRIVATE sim app platform_host hal core model o3os_qanet_rules_lib)
o3os_strict_warnings(o3_qanet_random_user)
add_dependencies(o3_qanet_random_user qanet_card_fixture)
set(_card ${_cards_dir}/qanet_card)
set(_base ${CMAKE_CURRENT_SOURCE_DIR}/baseline_random.txt)
add_test(NAME qanet_random_user_selftest COMMAND o3_qanet_random_user --selftest --card ${_card})
set_tests_properties(qanet_random_user_selftest PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAILED")
add_test(NAME qanet_random_user_short COMMAND o3_qanet_random_user --card ${_card} --seed 1 --seeds 2 --steps 300 --min-conclusive 1 --min-keys 1 --rules-every 1 --audio-probe --audio-probe-every 1 --meta --min-meta 1 --min-save-open 1 --found-dir ${CMAKE_BINARY_DIR}/qanet_found --baseline ${_base})
set_tests_properties(qanet_random_user_short PROPERTIES TIMEOUT 600)
# B2: a planted fault is shrunk to a short scenario, which fails under o3sim with the harness's message (found files go to the build's qanet_found/, never to git)
add_test(NAME qanet_shrink_selftest COMMAND o3_qanet_random_user --shrink-selftest --card ${_card} --found-dir ${CMAKE_BINARY_DIR}/qanet_found --o3sim $<TARGET_FILE:o3sim>)
set_tests_properties(qanet_shrink_selftest PROPERTIES TIMEOUT 900 FAIL_REGULAR_EXPRESSION "FAILED")
# C4: the audio probe's own proof (about 1 min under the sanitizers: two scripted walks, each replayed and probed four times)
add_test(NAME qanet_audio_probe_selftest COMMAND o3_qanet_random_user --audio-probe-selftest --card ${_card})
set_tests_properties(qanet_audio_probe_selftest PROPERTIES TIMEOUT 600 FAIL_REGULAR_EXPRESSION "FAILED")
add_test(NAME qanet_assert_selftest COMMAND o3_qanet_random_user --assert-selftest --card ${_card} --found-dir ${CMAKE_BINARY_DIR}/qanet_found --o3sim $<TARGET_FILE:o3sim>)
set_tests_properties(qanet_assert_selftest PROPERTIES TIMEOUT 600 FAIL_REGULAR_EXPRESSION "FAILED")
# B4: a rule-1 breach planted after a slot is filled is found at that step, shrunk and written
add_test(NAME qanet_random_rules_selftest COMMAND o3_qanet_random_user --rules-selftest --card ${_card} --found-dir ${CMAKE_BINARY_DIR}/qanet_found)
set_tests_properties(qanet_random_rules_selftest PROPERTIES TIMEOUT 900 FAIL_REGULAR_EXPRESSION "FAILED")
# B4b: a silent key that appears only after the document changed mid-walk (rule 2), and an out-of-span edit after a selection the walk made mid-walk (rule 3), are found, shrunk and written
add_test(NAME qanet_random_rules23_selftest COMMAND o3_qanet_random_user --rules23-selftest --card ${_card} --found-dir ${CMAKE_BINARY_DIR}/qanet_found)
set_tests_properties(qanet_random_rules23_selftest PROPERTIES TIMEOUT 1200 FAIL_REGULAR_EXPRESSION "FAILED")
add_dependencies(o3_qanet_random_user o3sim)
# B5: every metamorphic relation (save then OPEN, transpose and back, octave and back, copy + paste + undo, slice edit + undo) is reached, a save that loses a field is found, and the scenario written for it fails under o3sim
add_test(NAME qanet_random_meta_selftest COMMAND o3_qanet_random_user --meta-selftest --card ${_card} --found-dir ${CMAKE_BINARY_DIR}/qanet_found --o3sim $<TARGET_FILE:o3sim>)
set_tests_properties(qanet_random_meta_selftest PROPERTIES TIMEOUT 900 FAIL_REGULAR_EXPRESSION "FAILED")
# B8: the limit fixtures. The proof: each fixture (the undo cap, the sample-edit history cap) reaches its limit and finds nothing in a clean run, and a defect planted at the limit is found by rule 1. The short run: seeds 1 and 2
# (every fixture on both seeds, --exhaust-all: 4 runs of a fixture that must all reach their limit; the fixtures run after the walk, so the baseline holds as for the short run of these seeds).
add_test(NAME qanet_random_exhaust_selftest COMMAND o3_qanet_random_user --exhaust-selftest --card ${_card})
set_tests_properties(qanet_random_exhaust_selftest PROPERTIES TIMEOUT 900 FAIL_REGULAR_EXPRESSION "FAILED")
add_test(NAME qanet_random_exhaust COMMAND o3_qanet_random_user --card ${_card} --seed 1 --seeds 2 --steps 300 --min-conclusive 1 --min-keys 1 --rules-every 1 --audio-probe --audio-probe-every 1 --exhaust --exhaust-all --min-exhaust 6 --found-dir ${CMAKE_BINARY_DIR}/qanet_found --baseline ${_base})
set_tests_properties(qanet_random_exhaust PROPERTIES TIMEOUT 900)
# B8, the two fixtures that run on a simulator of their own (the walk is one step: it is not theirs): the voice pool saturated by held pads (seeds 1 and 2 draw other pads) and the folder of 2000 files. Their proof is the fresh self-test.
add_test(NAME qanet_random_exhaust_pool COMMAND o3_qanet_random_user --card ${_card} --seed 1 --seeds 2 --steps 1 --exhaust --exhaust-only voice_pool --min-exhaust 2 --found-dir ${CMAKE_BINARY_DIR}/qanet_found --baseline ${_base})
set_tests_properties(qanet_random_exhaust_pool PROPERTIES TIMEOUT 300)
add_test(NAME qanet_random_exhaust_folder COMMAND o3_qanet_random_user --card ${_card} --seed 1 --seeds 1 --steps 1 --exhaust --exhaust-only big_folder --min-exhaust 1 --found-dir ${CMAKE_BINARY_DIR}/qanet_found --baseline ${_base})
set_tests_properties(qanet_random_exhaust_folder PROPERTIES TIMEOUT 600)
add_test(NAME qanet_random_exhaust_fresh_selftest COMMAND o3_qanet_random_user --exhaust-fresh-selftest --card ${_card})
set_tests_properties(qanet_random_exhaust_fresh_selftest PROPERTIES TIMEOUT 900 FAIL_REGULAR_EXPRESSION "FAILED")
foreach(_i 1 2 3 4 5 6 7 8)
  add_test(NAME qanet_random_user_${_i}of8 COMMAND o3_qanet_random_user --card ${_card} --seed 1 --seeds 72 --steps 300 --rules-every 1 --min-keys 1 --min-sel 1 --env --shard ${_i}/8 --audio-session --meta --min-meta 1 --min-save-open 1 --found-dir ${CMAKE_BINARY_DIR}/qanet_found --baseline ${_base})
  set_tests_properties(qanet_random_user_${_i}of8 PROPERTIES TIMEOUT 14400)
endforeach()
# OS-QANET D1: the same walk on the FAT32 image of the same card (sim/CMakeLists.txt target card_images: o3fatimg), through the unit's FatFs (platform/host/fat): long names, FatFs's directory order, its
# handles. The same baseline: a finding that is on FatFs only is new, and so is a baseline line that FatFs no longer shows, except the lines of the rules (R0, R1 ...): this walk does not judge the rules (no --rules-every), so those lines belong to the folder run and their absence says nothing here. `ctest -R random_user_fatfs`. Short in `ci.sh host`, the 200 seeds in shards with the e2e label.
add_dependencies(o3_qanet_random_user card_images)
set(_card_fat ${_cards_dir}_fat/qanet_card.img)
add_test(NAME qanet_random_user_fatfs_short COMMAND o3_qanet_random_user --card ${_card_fat} --seed 1 --seeds 2 --steps 300 --min-conclusive 1 --baseline ${_base})
set_tests_properties(qanet_random_user_fatfs_short PROPERTIES TIMEOUT 600)
foreach(_i 1 2 3 4 5 6 7 8)
  add_test(NAME qanet_random_user_fatfs_${_i}of8 COMMAND o3_qanet_random_user --card ${_card_fat} --seed 1 --seeds 200 --steps 300 --env --shard ${_i}/8 --baseline ${_base})
  set_tests_properties(qanet_random_user_fatfs_${_i}of8 PROPERTIES TIMEOUT 14400)
endforeach()
# OS-QANET D3 (budgets.cpp): the latency budgets on the slow card (D2) over the FAT32 image (D1): a doctest binary of its own, ctest `qanet_budgets`, label app + qanet. The limits are one table in the source; the known
# findings (OS-BUG-PREVLAT) are in baseline_budgets.txt, and the run fails on a new one and on a line that no longer occurs.
add_executable(o3_qanet_budgets budgets.cpp)
target_include_directories(o3_qanet_budgets SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
target_link_libraries(o3_qanet_budgets PRIVATE core usb hal eve model formats platform_host platform_host_fat midi seq module o3m_builtin ui app sim formats_wav Threads::Threads ${CMAKE_DL_LIBS})
o3os_strict_warnings(o3_qanet_budgets)
add_dependencies(o3_qanet_budgets card_images)
target_compile_definitions(o3_qanet_budgets PRIVATE
  O3_QANET_CARD_IMAGE="${_card_fat}"
  O3_QANET_BUDGETS_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/baseline_budgets.txt")
add_test(NAME qanet_budgets COMMAND o3_qanet_budgets)
set_tests_properties(qanet_budgets PROPERTIES TIMEOUT 600 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
o3os_label_tests()
