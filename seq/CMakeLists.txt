# SPDX-License-Identifier: GPL-3.0-or-later
# seq: sequencer v2 (OS-005c, ADR 0013 D5): the pure `step(block_start, n) -> events[]` over a sample timeline, its FX table, the control layers, the row reader
# over the model's generation counters, and the transport, playheads and performance layer. Reads the project model, hands audio::SeqEvent to the voices and
# the MIDI out stage. Depends on core, model, audio (the event struct, the smoothing contract) and midi (the clock-follow loop it reads and the transport events it
# takes); nothing above it. No heap, no double, no printf, no hardware.
# OS-FLASHDIET P4: the play orders' entry table (src/play_orders_data.inc) deflated by tools/play_orders_pack.py into a generated blob in the build tree
# (kPlayOrdersZ). Every build links it (the tests unpack it and compare); only the MK66 image drops the plain table and reads the unpacked one (below).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_play_orders_blob_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/play_orders_blob.cpp")
add_custom_command(
  OUTPUT "${_play_orders_blob_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/play_orders_pack.py"
          --data "${CMAKE_CURRENT_SOURCE_DIR}/src/play_orders_data.inc" --out "${_play_orders_blob_gen}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/play_orders_data.inc"
          "${PROJECT_SOURCE_DIR}/tools/play_orders_pack.py" "${PROJECT_SOURCE_DIR}/tools/deflate_pack.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/seq/play_orders.h"
  COMMENT "Packing the play orders (tools/play_orders_pack.py)"
  VERBATIM)

o3os_library(seq
  SOURCES
    src/control.cpp
    src/midi_link.cpp
    src/offline.cpp
    src/perform.cpp
    src/play_orders.cpp "${_play_orders_blob_gen}"
    src/sequencer.cpp
    src/sequencer_lanes.cpp
    src/song_time.cpp
  DEPENDS core model audio midi)
if(O3OS_CROSS)
  target_compile_options(seq PRIVATE -fno-tree-loop-distribute-patterns)
  # E-SONG-07: the render's copy of the song list (OrderStore, 656 B) is the platform's, in SDRAM (SRAM_U keeps its 32 KiB free floor); the host's is inside the sequencer.
  target_compile_definitions(seq PUBLIC O3OS_SEQ_ORDER_EXTERNAL=1)
endif()
# OS-FLASHDIET P4: on the MK66 image the entries are read from the platform's SDRAM table (platform/mk66/src/app_main.cpp unpacks the blob into it in make_sound()),
# and the plain table is not linked (-2,176 B of flash for a 681 B blob). Host, simulator and QEMU keep the plain table.
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(seq PRIVATE O3OS_PLAY_ORDERS_PACKED=1)
endif()
if(O3OS_PLATFORM STREQUAL "host")
  target_compile_options(seq PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
