# SPDX-License-Identifier: GPL-3.0-or-later
# tracker-ce's CHIP engine (ChiptuneSynthVoice.cpp, MIT, Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition
# Contributors; THIRD_PARTY.md, PROVENANCE.md), vendored byte for byte, built for the HOST ONLY as the ORACLE of Chip's new waves (OS-CECHIP C1;
# tests/cechip_*_test.cpp, the dsp-port skill's gate 2). It is never linked into an image. The headers it includes are ours
# (tests/support/cechip_shim); the wrapper the tests use is tests/support/cechip_ref.{h,cpp}, built into the same library.
#   - the original is C++20 (designated initialisers, std::span, std::numbers): C++20 for this target only;
#   - no warnings on the original's file, no UBSan on it (its code, as for Open303), and -ffp-contract=off so that its float expressions stay as written;
#   - the wrapper is ours and keeps the project's warnings.
set(_o3_cechip_engine ${CMAKE_CURRENT_SOURCE_DIR}/ChiptuneSynthVoice.cpp)
set(_o3_cechip_wrapper ${PROJECT_SOURCE_DIR}/tests/support/cechip_ref.cpp)
add_library(cechip_oracle STATIC ${_o3_cechip_engine} ${_o3_cechip_wrapper})
set_target_properties(cechip_oracle PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(cechip_oracle PRIVATE ${PROJECT_SOURCE_DIR}/tests/support/cechip_shim)
target_compile_options(cechip_oracle PRIVATE -O2 -ffp-contract=off -fno-sanitize=undefined -fno-sanitize-recover=all)
set_source_files_properties(${_o3_cechip_engine} PROPERTIES COMPILE_OPTIONS "-w")
o3os_strict_warnings(cechip_oracle)

# The vendored files' SHA-256 check (ctest cechip_oracle_sha) is registered in tests/CMakeLists.txt: add_test belongs to the directory that
# runs after enable_testing(). The test script is check_sha256.cmake here, beside the files it checks.
