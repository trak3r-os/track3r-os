# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/update (OS-006c): the firmware update path over the `diag` console. Everything in it is pure and joins `platform_mk66_pure`, so the host
# tests and the QEMU suite run it: the path guard, the streaming Intel HEX validator, the transfer and arm state machines, the `fw` command and the
# FatFs-backed FirmwareStore, and since OS-FWCONFIG the adapter of the App's firmware port (fw_sink.cpp) (tests/mk66_update_*_test.cpp). Only src/fw_host_mk66.cpp (the PTA1 hand-over, the mute, the entropy) is the image's: the
# block of platform/mk66/CMakeLists.txt adds it to the executable.
target_sources(platform_mk66_pure PRIVATE src/path_guard.cpp src/hex_validator.cpp src/hex_pad.cpp src/fat_firmware_store.cpp src/fw_service.cpp src/fw_sink.cpp)
target_include_directories(platform_mk66_pure PUBLIC include)

# Every public header compiles on its own (the same check o3os_library() gives the layers).
file(GLOB_RECURSE _update_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
set(_update_stubs "")
foreach(header IN LISTS _update_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _update_stubs ${stub})
endforeach()
if(_update_stubs)
  add_library(platform_mk66_update_hdrcheck OBJECT ${_update_stubs})
  target_link_libraries(platform_mk66_update_hdrcheck PRIVATE platform_mk66_pure)
  o3os_strict_warnings(platform_mk66_update_hdrcheck)
endif()

# Host only: the validator against tools/check_image.py on the same files (the mutations of check_image's own self-test, and the real image when
# fw_hex_cross.py is given --hex). The helper prints one verdict per file; the script compares.
if(O3OS_PLATFORM STREQUAL "host")
  add_executable(o3os_fw_hex_verdict tools/fw_hex_verdict.cpp)
  target_link_libraries(o3os_fw_hex_verdict PRIVATE platform_mk66_pure)
  o3os_strict_warnings(o3os_fw_hex_verdict)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME mk66_fw_hex_cross
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fw_hex_cross.py
            --tool $<TARGET_FILE:o3os_fw_hex_verdict> --check-image ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_image.py)
  set_tests_properties(mk66_fw_hex_cross PROPERTIES TIMEOUT 120)
endif()

o3os_label_tests()
