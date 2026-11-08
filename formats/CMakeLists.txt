# SPDX-License-Identifier: GPL-3.0-or-later
# formats: byte-stream codecs over core::ByteSource / ByteSink into the model. formats/native is the project
# format (docs/formats/o3-project.md, OS-004a); formats/og reads and writes the Tracker OG's own files
# (docs/formats/og.md, OS-004b); format_registry is the registry the host side plugs its strategies into
# (ADR 0009 rev 1; builtin_formats.cpp, the registry's own strategies, is the host-only formats_builtin library below);
# wav_sample.cpp makes the WAV importer of formats/wav (its own library, OS-002c) a registered sample format.
# settings_chunks / settings_file (OS-002d): the SETS and LRNB chunks of settings, shared by the project format and the device settings file
# (docs/formats/o3-settings.md); preset_file.cpp (OS-MODPRESET P1): the module preset file O3MP, one MPST chunk (docs/formats/o3-preset.md); iff_sample.cpp (OS-SAMPFMT): AIFF / AIFF-C and IFF 8SVX as one more registered sample format. Depends on core, model and formats_wav.
# volume_level.cpp is not here but in formats_tracker: only the MOD / MED mapping calls it.
# tracker_sniff.cpp (OS-FLASHDIET I3): the probe side of the MOD / S3M / MED importers — mod::sniff_kind with tagged_mod_min_bytes and
# is_soundtracker15, and the mod / med drop_group_name tables — which the image needs (app/module_import.cpp) without linking the
# importers; they live in the formats_tracker library below.
o3os_library(formats
  SOURCES src/crc32.cpp src/native_reader.cpp src/native_writer.cpp src/native_state.cpp
          src/format_registry.cpp src/wav_sample.cpp src/wt_surge.cpp src/iff_sample.cpp src/sample_probe.cpp
          src/settings_chunks.cpp src/settings_file.cpp src/preset_file.cpp src/tracker_sniff.cpp
          src/og_common.cpp src/og_project.cpp src/og_pattern.cpp src/og_instrument.cpp src/og_pamd.cpp
  DEPENDS core model formats_wav)

# The MOD / S3M / MED importers' headers are public headers of this layer (app/module_import.h includes them for the probe), but their
# code is the formats_tracker library below: this include path is headers only, no symbol of formats_tracker is linked from here.
target_include_directories(formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mod/include ${CMAKE_CURRENT_SOURCE_DIR}/med/include)

# OS-FLASHDIET I3: the MOD / S3M / MED importers (docs/formats/mod.md, OS-MODIMP; docs/formats/med.md, OS-008a/b) as their own library,
# formats_tracker, that the importer module (modules/importers/tracker/tmod), the tests and the host registry (formats_builtin) link —
# NOT the image: from this slice the image reads those files only through the importer module of the card. formats_tracker links formats
# (the probe side above, and the model / core calls), so its objects resolve against the image's library too.
set(FORMATS_TRACKER_SOURCES
    mod/src/module.cpp mod/src/flow.cpp mod/src/map.cpp mod/src/import.cpp
    med/src/mmd_reader.cpp med/src/med_tempo.cpp med/src/med_flow.cpp med/src/med_map.cpp med/src/med_import.cpp
    src/volume_level.cpp)
add_library(formats_tracker STATIC ${FORMATS_TRACKER_SOURCES})
set_target_properties(formats_tracker PROPERTIES OUTPUT_NAME o3os_formats_tracker)
target_include_directories(formats_tracker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/mod/include ${CMAKE_CURRENT_SOURCE_DIR}/med/include)
target_link_libraries(formats_tracker PUBLIC formats)
o3os_strict_warnings(formats_tracker)
target_compile_options(formats_tracker PRIVATE -fsingle-precision-constant)
if(O3OS_PLATFORM STREQUAL "mk66")
  add_custom_command(TARGET formats_tracker POST_BUILD
    COMMAND ${CMAKE_SIZE} -t $<TARGET_FILE:formats_tracker>
    VERBATIM)
endif()
# The mod and med headers get their own stubs (the generic header check above covers formats/include only): each is compiled on its own.
set(_tracker_stubs "")
foreach(_dir mod med)
  file(GLOB_RECURSE _tracker_headers CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/include
       ${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/include/*.h)
  foreach(header ${_tracker_headers})
    string(MAKE_C_IDENTIFIER ${_dir}_${header} id)
    set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
    file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
    list(APPEND _tracker_stubs ${stub})
  endforeach()
endforeach()
if(_tracker_stubs)
  add_library(formats_tracker_hdrcheck OBJECT ${_tracker_stubs})
  target_link_libraries(formats_tracker_hdrcheck PRIVATE formats_tracker)
  o3os_strict_warnings(formats_tracker_hdrcheck)
  target_compile_options(formats_tracker_hdrcheck PRIVATE -fsingle-precision-constant)
endif()
# The module packer (modules/CMakeLists.txt, the tracker module) builds these same sources: one list, kept here.
set_property(GLOBAL PROPERTY O3OS_FORMATS_TRACKER_SOURCES "${FORMATS_TRACKER_SOURCES}")

# OS-FLASHDIET I3: the registry of the built-in formats (builtin_formats.cpp, the Strategy objects of every built-in format including
# the MOD / S3M / MED importers) is the host side's library: it links formats_tracker, and the image never linked it (nothing outside
# the tests references the registry), so no formats_tracker code reaches the image through it.
add_library(formats_builtin STATIC src/builtin_formats.cpp)
set_target_properties(formats_builtin PROPERTIES OUTPUT_NAME o3os_formats_builtin)
target_link_libraries(formats_builtin PUBLIC formats formats_tracker)
o3os_strict_warnings(formats_builtin)
target_compile_options(formats_builtin PRIVATE -fsingle-precision-constant)
