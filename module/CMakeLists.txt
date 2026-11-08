# SPDX-License-Identifier: GPL-3.0-or-later
# module: the loader of native modules from the SD card (ADR 0002, OS-012a): the file format's checks, the relocation of the image
# into two places (hot code in the SRAM_L slab, the rest in SDRAM), the stepped file reader and the set of loaded modules. The SDK
# header (sdk/o3module.h, MIT) is the contract with the modules themselves. Depends on core and hal (the storage contract), and on model
# for the importer layout tag (OS-FLASHDIET I1: model/import_layout.h, the word 44 a kind 3 file must carry); the audio
# engine runs the loaded modules (audio/module_voices.h) and depends on this; the layer does not know the engine. No heap, no double.
o3os_library(module
  SOURCES
    src/builtin_notices.cpp
    src/image.cpp
    src/loader.cpp
    src/ptfx_format.cpp
    src/ptfx_catalog.cpp
    src/ptfx_loader.cpp
    src/ptfx_host_table.cpp
    src/view.cpp
  DEPENDS core hal model)
# OS-CEPTFX S2a: ptfx_host_sqrtf must be one VSQRT with no libm / errno fallback reachable from the plugin's context.
set_source_files_properties(src/ptfx_host_table.cpp PROPERTIES COMPILE_OPTIONS "-fno-math-errno")
target_include_directories(module PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
