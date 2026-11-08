# SPDX-License-Identifier: GPL-3.0-or-later
# modules/ptfx: the .ptfx Sample Editor plugins this repository builds for the SD card's Effects/ folder (OS-PTFXPACK, docs/specs/OS-CEPTFX.md 6).
# Each plugin is ONE C file against the SDK header sdk/tracker_plugin.h, with its header fields in the source (the `// PTFX_*` lines and PLUGIN_PARAMS[]);
# tools/ptfx_pack.py compiles it for the Cortex-M4F with the flags of the .ptfx format (hard-float, -fPIC, long calls, no libc), links it at 0, writes the
# 472-byte header and reads the file back through the checks of tools/ptfx_inspect.py. The result, <build>/modules/ptfx/<stem>.ptfx, is a build product that is
# never committed and never part of a firmware image: nothing here is linked into any firmware target, and `make_starter.py` (tools/release/README.md) is what
# copies the files into the SD content zip.
#   - add_silence, analog_kick_gen, chiptune_gen, multimode_filter, stereo_delay, stereo_to_mono: tracker-ce@7fd6c2c editor-fx-sdk/effects, taken as is (MIT, notice at
#     the top of each file, LICENSE-tracker-ce.txt, THIRD_PARTY.md);
#   - overdrive, delay, bitcrusher, chorus, flanger, limiter, eq3: our rebuilds of the same-named effects of tracker-ce's core/effector (P2, P3): a port of each algorithm, see
#     the header of each file for what is the original's and what is changed. Not rebuilt: Wavetable Smoother, Cut, Time-stretch (tools/release/README.md).
# Needs arm-none-eabi-gcc like the .tmod targets (O3M_ARM_CXX of modules/CMakeLists.txt): without it no .ptfx target exists and ctest ptfx_pack says so.
if(O3OS_CROSS)
  return()  # the firmware builds (mk66, qemu) have no use for the plugins: the host build makes them, so no image build step changes
endif()
if(NOT O3M_ARM_CXX)
  message(STATUS "ptfx: no arm-none-eabi-g++ (set ARM_GNU_ROOT): the .ptfx targets are not made")
  return()
endif()

set(O3_PTFX_PACK "${PROJECT_SOURCE_DIR}/tools/ptfx_pack.py")
set(O3_PTFX_INSPECT "${PROJECT_SOURCE_DIR}/tools/ptfx_inspect.py")
set(O3_PTFX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
string(REGEX REPLACE "g\\+\\+(\\.exe)?$" "" O3_PTFX_PREFIX "${O3M_ARM_CXX}")  # .../arm-none-eabi- , the prefix ptfx_pack.py takes as --toolchain

set(O3_PTFX_STEMS add_silence analog_kick_gen chiptune_gen multimode_filter stereo_delay stereo_to_mono
    overdrive delay bitcrusher chorus flanger limiter eq3)
set(O3_PTFX_OUTPUTS)
foreach(stem IN LISTS O3_PTFX_STEMS)
  set(src "${CMAKE_CURRENT_SOURCE_DIR}/${stem}/${stem}.c")
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${stem}.ptfx")
  add_custom_command(OUTPUT "${out}"
    COMMAND ${Python3_EXECUTABLE} -I ${O3_PTFX_PACK} ${src} -o ${out} --abi-dir ${O3_PTFX_SDK_DIR} --toolchain ${O3_PTFX_PREFIX}
    DEPENDS ${src} ${O3_PTFX_SDK_DIR}/tracker_plugin.h ${O3_PTFX_PACK} ${O3_PTFX_INSPECT}
    COMMENT "ptfx_pack ${stem}.ptfx"
    VERBATIM)
  list(APPEND O3_PTFX_OUTPUTS "${out}")
endforeach()
add_custom_target(o3_ptfx ALL DEPENDS ${O3_PTFX_OUTPUTS})

# The checks: every built file parses as Ok through tools/ptfx_inspect.py with its display name, is exactly 472 + code_size bytes, and each plugin source runs on the host
# (compiled natively against a stand-in host table) for a smoke test; the vendored files are byte-identical to the pinned upstream ones past their notice.
add_test(NAME ptfx_pack COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/ptfx_pack_test.py ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${O3_PTFX_PREFIX})
set_tests_properties(ptfx_pack PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

o3os_label_tests()  # ptfx_pack gets its area label (cmake/O3osLabels.cmake)
