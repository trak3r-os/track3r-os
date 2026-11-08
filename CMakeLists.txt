# SPDX-License-Identifier: GPL-3.0-or-later
#
# Community OS: standalone project, see docs/specs/OS-001-walking-skeleton.md.
#
#   host:  cmake -S . -B build-os-host -DO3OS_SANITIZE=ON
#          cmake --build build-os-host && ctest --test-dir build-os-host --output-on-failure
#   mk66:  cmake -S . -B build-os-mk66 -DO3OS_PLATFORM=mk66 \
#              -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake -DARM_GNU_ROOT=<arm-gnu-toolchain dir>
#          cmake --build build-os-mk66      (the static libraries, with arm-none-eabi-size per library, and the skeleton image
#                                            o3os.elf/.hex/.bin of platform/mk66 behind its static checks (OS-007b); finds the
#                                            toolchain from ARM_GNU_ROOT (or PATH), see docs/toolchain.md)
#          ctest --test-dir build-os-mk66   (the image checks: HEX validity, address range, vectors, flash configuration)
#   qemu:  cmake -S . -B build-os-qemu -DO3OS_PLATFORM=qemu \
#              -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake -DARM_GNU_ROOT=<arm-gnu-toolchain dir>
#          cmake --build build-os-qemu && ctest --test-dir build-os-qemu     (OS-007a: the unit-test suites
#                                            cross-built for the Cortex-M4F and run on QEMU mps2-an386)
#
# Layering (enforced by CMake link dependencies and tools/check_layers.py):
#   core <- usb <- hal <- eve <- ui <- app;  platform/<name> and sim implement the HAL contracts.
#   core <- model <- formats; app (and sim, tests) may use model and formats; hal, eve and ui may not.
#   formats/wav (OS-002c) is a library of its own in the formats layer: core only.
cmake_minimum_required(VERSION 3.20)

set(O3OS_PLATFORM "host" CACHE STRING "Target platform: host, mk66 or qemu")
set_property(CACHE O3OS_PLATFORM PROPERTY STRINGS host mk66 qemu)
if(NOT O3OS_PLATFORM MATCHES "^(host|mk66|qemu)$")
  message(FATAL_ERROR "O3OS_PLATFORM must be 'host', 'mk66' or 'qemu' (got '${O3OS_PLATFORM}')")
endif()

# mk66 and qemu are both bare-metal Cortex-M4F cross builds with the same flags (the QEMU target tests, OS-007a, run
# the MK66's code generation on a model of the same core).
if(O3OS_PLATFORM MATCHES "^(mk66|qemu)$")
  set(O3OS_CROSS TRUE)
else()
  set(O3OS_CROSS FALSE)
endif()

# The cross builds use cmake/mk66-toolchain.cmake (arm-none-eabi; the compiler is found through ARM_GNU_ROOT or PATH, see
# cmake/arm-none-eabi-toolchain.cmake). It must be set before project().
if(O3OS_CROSS AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/mk66-toolchain.cmake")
endif()

# C as well as C++ (OS-007e): the one C file in the tree is the vendored FatFs (third_party/fatfs); everything first-party is C++.
project(o3os VERSION 0.1.0 LANGUAGES C CXX)

if(O3OS_CROSS AND NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
  message(FATAL_ERROR "O3OS_PLATFORM=${O3OS_PLATFORM} needs an arm-none-eabi toolchain file (cmake/mk66-toolchain.cmake)")
endif()

option(O3OS_SANITIZE "Host only: build everything with AddressSanitizer + UBSan" OFF)
option(O3OS_TSAN "Host only: build everything with ThreadSanitizer (excludes O3OS_SANITIZE)" OFF)
option(O3OS_WERROR "Treat warnings as errors" ON)
# D-OS-8 (ADR 0012 amendment 2): the audio sample clock. ON (default since v1.0.0, verified on the unit 2026-10-23): 1800000/441
# cycles per sample, MCLK = 180 MHz x 196/3125, exactly 44,100 Hz. OFF: 4080 CPU cycles per sample, MCLK = 180 MHz x 16/255,
# 44,117.647 Hz (+400 ppm against 44,100), the pre-1.0 clock. It changes ONE constant (core/audio_clock.h, which alone reads the definition); the I2S divider, the block
# length, the tone, the USB feedback value and every test follow, and every target gets the same value.
option(O3OS_RATE_44100 "Audio clock retuned to exactly 44,100 Hz (D-OS-8): 1800000/441 cycles per sample instead of 4080/1 (OFF builds the pre-1.0 44,117.647 Hz clock)" ON)
if(O3OS_RATE_44100)
  add_compile_definitions(O3OS_RATE_44100=1)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # mk66 (and qemu) follow the firmware's size profile (-Os); host defaults to unoptimised for the sanitizers.
  if(O3OS_CROSS)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
  else()
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)            # -std=gnu++17
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every target, host included, is built without RTTI and exceptions. (-fno-rtti is C++ only: gcc warns when it is given to a C file.)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti> -fno-exceptions)

if(O3OS_CROSS)
  # Same as the firmware's lean profile (root CMakeLists.txt): bare metal, no threads, so no guard
  # variables around function-local statics.
  add_compile_options(-ffunction-sections -fdata-sections $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
  if(O3OS_SANITIZE OR O3OS_TSAN)
    message(FATAL_ERROR "sanitizers are host only")
  endif()
endif()

if(O3OS_SANITIZE AND O3OS_TSAN)
  message(FATAL_ERROR "O3OS_SANITIZE and O3OS_TSAN are mutually exclusive")
elseif(O3OS_SANITIZE)
  # vptr is already off with -fno-rtti; named explicitly so a toolchain that checks it cannot object.
  set(_san -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  # -O1 over the Debug default (-O0; owner, 2026-10-14): what the ASan documentation recommends; the same checks, faster suites.
  add_compile_options(${_san} -O1)
  add_link_options(${_san})
elseif(O3OS_TSAN)
  # -Wno-error=tsan: GCC warns that TSan does not model std::atomic_thread_fence (the seqlocks of midi/config.h, audio/taps.h); the first-party -Werror would stop the build on it.
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -Wno-error=tsan)
  add_link_options(-fsanitize=thread)
endif()

include(cmake/O3osTargets.cmake)
include(cmake/O3osLabels.cmake)  # o3os_label_tests(): the area label of every ctest, called at the end of each directory that adds tests

# OS-007e: FatFs (ChaN R0.15a, read-only configuration), the file system of the SD card. A third-party static library of its own, built
# on every platform because platform/mk66's pure parts (the FatFs-backed hal::storage::Storage) and their tests run on the host and on QEMU.
add_subdirectory(third_party/fatfs)

# OS-007b: platform/mk66 registers ctests (its python tools' self-tests, the image checks) in every build that includes it,
# so testing is enabled before its directory is added, whatever the platform.
enable_testing()

# OS-007a: the target test layer. enable_testing() comes first so platform/qemu can register its own suites.
if(O3OS_PLATFORM STREQUAL "qemu")
  enable_testing()
  include(cmake/O3osQemu.cmake)
endif()

add_subdirectory(core)
add_subdirectory(usb)
add_subdirectory(hal)
add_subdirectory(module)  # OS-012a: the SD module loader (below audio, which runs the modules)
add_subdirectory(audio)   # OS-005a: the pure audio engine skeleton (beside the chain, like model)
add_subdirectory(midi)    # OS-005c: the MIDI timing core (above audio's event struct, below the sequencer)
add_subdirectory(eve)
add_subdirectory(model)
add_subdirectory(seq)     # OS-005c: sequencer v2 (reads the model, feeds audio's event struct)
add_subdirectory(formats/wav)  # OS-002c: the WAV sample importer, a library of its own (core only), used by formats
add_subdirectory(formats)
add_subdirectory(modules)  # OS-012a: the modules this repository builds (host shared objects, .tmod files for the card)

# OS-001b adds ui/, app/, platform/mk66 and sim/ as plain directories; nothing here needs to change.
foreach(_layer ui app)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_layer}/CMakeLists.txt")
    add_subdirectory(${_layer})
  endif()
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platform/${O3OS_PLATFORM}/CMakeLists.txt")
  add_subdirectory(platform/${O3OS_PLATFORM})
endif()

# OS-007b: the MK66 platform's pure parts (memory map, crash record, memory tests, clock and SDRAM register values) build on
# the host and QEMU too, so that their tests run there; the image itself is built only with -DO3OS_PLATFORM=mk66, which
# adds the directory above.
if(NOT O3OS_PLATFORM STREQUAL "mk66")
  add_subdirectory(platform/mk66)
endif()

# OS-007a: tests/ registers each library's target suite (see tests/CMakeLists.txt and README.md).
if(O3OS_PLATFORM STREQUAL "qemu")
  add_subdirectory(tests)
endif()

if(O3OS_PLATFORM STREQUAL "host")
  # OS-007c: the pinned Riedel EVE library, as a host-only oracle for the display driver's tests.
  add_subdirectory(third_party/eve-riedel)
  # OS-012a: Open303, the TB-303 module's double-precision oracle (host only).
  add_subdirectory(third_party/open303)
  # OS-005h: pt2-clone's Paula emulator, the Paula model's oracle (host only).
  add_subdirectory(third_party/pt2-clone)
  # OS-CECHIP C1: tracker-ce's CHIP engine, the oracle of Chip's new waves (host only).
  add_subdirectory(third_party/tracker-ce-chip)

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sim/CMakeLists.txt")
    add_subdirectory(sim)
  endif()

  enable_testing()
  add_subdirectory(tests)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_check "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layers.py")
  add_test(NAME layers COMMAND ${Python3_EXECUTABLE} -I ${_check} --root ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME layers_selftest COMMAND ${Python3_EXECUTABLE} -I ${_check} --self-test)
  # OS-KEYROWDLG S4: a function key is named in one table (app/src/key_prompt.cpp): no other string of app/ or the image's main may say F1..F8 / SHIFT+F, but the listed pointers.
  set(_keytext "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_keytext.py")
  add_test(NAME keytext COMMAND ${Python3_EXECUTABLE} -I ${_keytext} --root ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME keytext_selftest COMMAND ${Python3_EXECUTABLE} -I ${_keytext} --self-test)
endif()

o3os_label_tests()
