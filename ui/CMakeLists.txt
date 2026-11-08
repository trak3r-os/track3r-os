# SPDX-License-Identifier: GPL-3.0-or-later
# ui: immediate-mode renderer on eve, the colour tokens and themes, the faces and bitmap assets, the components.
# Depends on eve (and through it hal).
#
# The face and icon data are generated at build time into the build tree and compiled into this library; they are never
# committed. Faces: tools/font_convert.py turns the vendored BDF files into the four glyph faces (Terminus 16x32 and
# 10x20, SIL OFL 1.1; Spleen 8x16 and 6x12, BSD-2-Clause); icons: tools/icon_convert.py draws the in-house icon face.
# Both emit word-aligned bitmaps with a static_assert (ADR 0011). OS-FLASHDIET A4: font_convert.py emits each face as a
# raw-deflate stream of its bitmap (tools/deflate_pack.py), decoded at boot by core::Inflater. Tiles are not generated
# here: they are composed from the Data face at boot (src/assets.cpp).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_spleen_dir "${PROJECT_SOURCE_DIR}/third_party/fonts/spleen")
set(_terminus_dir "${PROJECT_SOURCE_DIR}/third_party/fonts/terminus")
set(_font_display "${_terminus_dir}/ter-u32b.bdf")
set(_font_data "${_terminus_dir}/ter-u20b.bdf")
set(_font_label "${_spleen_dir}/spleen-8x16.bdf")
set(_font_micro "${_spleen_dir}/spleen-6x12.bdf")
set(_font_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/font_data.cpp")
set(_icon_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/icon_data.cpp")

add_custom_command(
  OUTPUT "${_font_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/font_convert.py" --out "${_font_gen}"
          --face "f16x32=${_font_display}" --face "f10x20=${_font_data}"
          --face "f8x16=${_font_label}" --face "f6x12=${_font_micro}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/font_convert.py" "${PROJECT_SOURCE_DIR}/tools/deflate_pack.py"
          "${_font_display}" "${_font_data}" "${_font_label}" "${_font_micro}"
  COMMENT "Converting the BDF faces to FT81x L1 font data (deflated, OS-FLASHDIET A4)"
  VERBATIM)

add_custom_command(
  OUTPUT "${_icon_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/icon_convert.py" --out "${_icon_gen}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/icon_convert.py"
  COMMENT "Converting the in-house icons to FT81x L1 data"
  VERBATIM)

o3os_library(ui
  SOURCES src/renderer.cpp src/assets.cpp src/themes.cpp src/components.cpp "${_font_gen}" "${_icon_gen}"
          src/boot_screen.cpp src/boot_logo_data.cpp
  DEPENDS eve)

# OS-FLASHDIET A4: on the MK66 image the assets' shared decoder (core::Inflater) and the 4 KiB face scratch live in
# SDRAM_STATIC and are defined by the platform (platform/mk66/src/app_main.cpp); this drops ui's own copies in
# assets.cpp. Host, the simulator and QEMU keep ui's own.
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(ui PRIVATE O3OS_UI_ASSETS_EXTERNAL=1)
endif()

# OS-003f, the boot logo: assets/boot_logo.png is the project's logo.png byte for byte; tools/logo_convert.py turns it into
# src/boot_logo_data.cpp, which IS committed (unlike the font and icon data above, which are generated into the build tree): the
# converter takes seconds and the picture changes about never, so a build does not run it. The ctest `logo_convert_fresh` (host
# tree) regenerates it in memory and fails when the committed file differs; regenerate with
#   python3 -I tools/logo_convert.py --png ui/assets/boot_logo.png --out ui/src/boot_logo_data.cpp

# ---- ui::plot (OS-003d): part of ui, sources and headers under plot/ (ADR 0010 amendment 2) ----
# Peak-cache mipmap, wave view (vector and bitmap), curves, XY scope, spectrum, meters, the RAM_G map. They draw through
# ui::Renderer's raw sections and are built into the same library, so the MK66 `ui` size includes them.
target_sources(ui PRIVATE
  plot/src/peak_cache.cpp plot/src/plot_math.cpp plot/src/wave_bitmap.cpp plot/src/wave_view.cpp plot/src/curve.cpp
  plot/src/fft.cpp plot/src/spectrum.cpp plot/src/scope.cpp plot/src/meters.cpp)
target_include_directories(ui PUBLIC plot/include)
file(GLOB_RECURSE _plot_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/plot/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/plot/include/*.h")
set(_plot_stubs "")
foreach(header IN LISTS _plot_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_plot/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _plot_stubs ${stub})
endforeach()
if(_plot_stubs)  # every plot header compiles on its own, like the rest of ui's
  add_library(ui_plot_hdrcheck OBJECT ${_plot_stubs})
  target_link_libraries(ui_plot_hdrcheck PRIVATE ui)
  o3os_strict_warnings(ui_plot_hdrcheck)
  target_compile_options(ui_plot_hdrcheck PRIVATE -fsingle-precision-constant)
endif()

if(O3OS_PLATFORM STREQUAL "host")
  enable_testing()
  add_test(NAME font_convert_selftest
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/font_convert.py" --self-test
            --spleen-dir "${_spleen_dir}" --terminus-dir "${_terminus_dir}")
  add_test(NAME icon_convert_selftest
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/icon_convert.py" --self-test)
  # OS-003f: the logo converter (PNG decoder on every filter and colour type, grid detection, quantisation, the output) and the
  # staleness check of the committed boot_logo_data.cpp against assets/boot_logo.png.
  add_test(NAME logo_convert_selftest
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/logo_convert.py" --self-test)
  add_test(NAME logo_convert_fresh
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/logo_convert.py"
            --png "${CMAKE_CURRENT_SOURCE_DIR}/assets/boot_logo.png" --check "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_logo_data.cpp")
  set_tests_properties(logo_convert_selftest logo_convert_fresh PROPERTIES TIMEOUT 120)
endif()

o3os_label_tests()
