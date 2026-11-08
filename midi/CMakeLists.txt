# SPDX-License-Identifier: GPL-3.0-or-later
# midi: the MIDI timing core (OS-005c, ADR 0013 D5): pure, host-tested and cross-built, no hardware access (the MK66 drivers' registers are platform/mk66, OS-005c2; what their
# interrupts do is here: din_io.h, usb_bind.h, over a register policy, with the time base they share, time_ref.h, and the settings value, config.h). Messages and the
# byte-time model of DIN, the timed queue, the DIN emitter's "clock is sacred" state machine, the USB-MIDI 1 ms frame quantiser, the note-hang bookkeeping and the
# out router; the clock-follow DLL with its stamp map, and the MIDI in parser with the transport events. Depends on core (the rings, Q32 time), hal and usb (the
# contracts it will be bound to) and audio (the event struct and the hot-path placement macro); nothing above it. No heap, no double, no printf.
o3os_library(midi
  SOURCES
    src/din.cpp
    src/usb_out.cpp
    src/clock_dll.cpp
    src/in.cpp
    src/out.cpp
    src/mtc.cpp
    src/mtc_in.cpp
    src/usb_bind.cpp
  DEPENDS core usb hal audio)
target_compile_options(midi PRIVATE -fno-math-errno)
# The real-time functions (O3OS_AUDIO_HOT: the emitter's poll, the DLL update, the drains) run from SRAM_L on the MK66, and may not call into flash (tools/check_ramfunc.py):
# a zeroing or copying loop must stay a loop.
if(O3OS_CROSS)
  target_compile_options(midi PRIVATE -fno-tree-loop-distribute-patterns)
endif()
if(O3OS_PLATFORM STREQUAL "host")
  target_compile_options(midi PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
