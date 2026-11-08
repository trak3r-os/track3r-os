# SPDX-License-Identifier: GPL-3.0-or-later
# platform/qemu: the target test layer for QEMU's `mps2-an386` (Cortex-M4F): startup, linker script, semihosting
# console and exit code, a SysTick hal::Clock, newlib stubs and the doctest runner. Built only with
# -DO3OS_PLATFORM=qemu; never linked by core..app. Registering a suite is cmake/O3osQemu.cmake's
# o3os_target_suite().

# Object libraries, so the startup code and the newlib stubs are always linked in (an archive member nothing names
# before libc is scanned would be dropped).
add_library(platform_qemu OBJECT src/startup.cpp src/syscalls.cpp src/semihost.cpp src/clock.cpp src/runtime.cpp)
target_include_directories(platform_qemu PUBLIC include)
target_link_libraries(platform_qemu PUBLIC hal audio)
o3os_strict_warnings(platform_qemu)
# The reset sequence is the C runtime: its copy and zero loops must stay loops, not become memcpy / memset calls.
set_source_files_properties(src/startup.cpp PROPERTIES COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)

# What every image links with: our linker script and startup instead of newlib's crt0, nano libc and libstdc++ with
# the system calls from src/syscalls.cpp (no libnosys), unused sections dropped, and float formatting for failure
# messages.
set(_ld "${CMAKE_CURRENT_SOURCE_DIR}/mps2_an386.ld")
target_link_options(platform_qemu INTERFACE
  -nostartfiles -T${_ld} --specs=nano.specs -Wl,--gc-sections -Wl,-u,_printf_float)
set_property(TARGET platform_qemu APPEND PROPERTY INTERFACE_LINK_DEPENDS ${_ld})

# Every public header also compiles on its own (as o3os_library() does for the layers).
file(GLOB_RECURSE _headers CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
set(_stubs "")
foreach(_header IN LISTS _headers)
  string(MAKE_C_IDENTIFIER ${_header} _id)
  set(_stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${_id}.cpp")
  file(CONFIGURE OUTPUT ${_stub} CONTENT "#include <@_header@>\n" @ONLY)
  list(APPEND _stubs ${_stub})
endforeach()
add_library(platform_qemu_hdrcheck OBJECT ${_stubs})
target_link_libraries(platform_qemu_hdrcheck PRIVATE platform_qemu)
o3os_strict_warnings(platform_qemu_hdrcheck)

# doctest's implementation and our main(), compiled once for all suites. The defines are PUBLIC: they select the
# same configuration of <doctest/doctest.h> in the test files (no signals and no threads on bare metal).
add_library(platform_qemu_runner OBJECT src/test_runner.cpp)
target_link_libraries(platform_qemu_runner PUBLIC platform_qemu)
target_include_directories(platform_qemu_runner SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
target_compile_definitions(platform_qemu_runner PUBLIC DOCTEST_CONFIG_NO_POSIX_SIGNALS DOCTEST_CONFIG_NO_MULTITHREADING)
o3os_strict_warnings(platform_qemu_runner)

# The layer's own suite: what only a target can show (reset sequence, FPU, SysTick clock, heap).
o3os_target_suite(platform SOURCES tests/startup_test.cpp)
# OS-CEPTFX S2b: the call gate (hal/code_gate.h) on the real Cortex-M4F, with our own Thumb test plugins: every
# containment path (return, timeout, fault, stack overflow, register clobber) plus the canary after each. S3b adds the
# run job of the app (app/src/ptfx_job.cpp, a static library: only that object and what it uses are linked).
o3os_target_suite(ptfx_gate DEPENDS app module hal core SOURCES tests/ptfx_gate_test.cpp TIMEOUT 120)
# The probes: test cases that fail on purpose. `qemu_probe` passes only if a failed CHECK comes out as QEMU's exit
# status 1, `qemu_probe_stack` only if a stack overflow made of sparse 1 KiB frames (the case a few painted guard words
# cannot catch) is stopped by the MPU guard with status 3 (see O3osQemuExpectFailure.cmake). Together they prove that
# a failing suite fails ctest.
o3os_target_suite(probe SOURCES tests/failure_probe.cpp EXPECT_FAILURE test_failure TIMEOUT 30)
o3os_target_suite(probe_stack SOURCES tests/stack_overflow_probe.cpp EXPECT_FAILURE stack_overflow TIMEOUT 30)

# OS-005a: the fake I2S/DMA/codec (a hal::audio::Device whose block clock is a CMSDK timer interrupt) and the suite that runs the
# engine skeleton on it. Linked only by that suite: it defines the render line (PendSV) and the timer handler, and hal::cycles().
add_library(platform_qemu_audio OBJECT src/fake_i2s.cpp)
target_include_directories(platform_qemu_audio PUBLIC include)
target_link_libraries(platform_qemu_audio PUBLIC platform_qemu audio hal)
o3os_strict_warnings(platform_qemu_audio)
o3os_target_suite(audio_fake DEPENDS platform_qemu_audio audio SOURCES tests/audio_fake_test.cpp TIMEOUT 240 VIRTUAL_IDLE)
# OS-005b: the engine is 36 KiB, too big for the target's test stack; the suite builds it in tests/support/static_slot.h's storage.
target_include_directories(audio_fake_suite PRIVATE ${PROJECT_SOURCE_DIR}/tests)
o3os_label_tests()
