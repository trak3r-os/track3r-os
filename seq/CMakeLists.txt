# SPDX-License-Identifier: GPL-3.0-or-later
# seq: sequencer v2 (OS-005c, ADR 0013 D5): the pure `step(block_start, n) -> events[]` over a sample timeline, its FX table, the control layers, the row reader
# over the model's generation counters, and the transport, playheads and performance layer. Reads the project model, hands audio::SeqEvent to the voices and
# the MIDI out stage. Depends on core, model, audio (the event struct, the smoothing contract) and midi (the clock-follow loop it reads and the transport events it
# takes); nothing above it. No heap, no double, no printf, no hardware.
o3os_library(seq
  SOURCES
    src/control.cpp
    src/midi_link.cpp
    src/offline.cpp
    src/perform.cpp
    src/sequencer.cpp
    src/song_time.cpp
  DEPENDS core model audio midi)
if(O3OS_CROSS)
  target_compile_options(seq PRIVATE -fno-tree-loop-distribute-patterns)
  # E-SONG-07: the render's copy of the song list (OrderStore, 656 B) is the platform's, in SDRAM (SRAM_U keeps its 32 KiB free floor); the host's is inside the sequencer.
  target_compile_definitions(seq PUBLIC O3OS_SEQ_ORDER_EXTERNAL=1)
endif()
if(O3OS_PLATFORM STREQUAL "host")
  target_compile_options(seq PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
