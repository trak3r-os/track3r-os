# SPDX-License-Identifier: GPL-3.0-or-later
# hal: contracts only (Display, Input, Clock, the interrupt priority table, the audio device, the read-only SD card storage), plus, on the
# cross builds only, the one Cortex-M4F source both targets share: the OS-CEPTFX call gate (src/code_gate_m4.cpp, the ISR/fault-path code of
# docs/specs/OS-CEPTFX.md 3.3). On the host the gate's header carries an inline direct-call implementation for the tests, so `hal` stays
# headers only there. Depends on core alone: the one sample-rate constant the contracts need is core/audio_clock.h's (ADR 0012 am. 2).
if(O3OS_CROSS)
  o3os_library(hal SOURCES src/code_gate_m4.cpp DEPENDS core)
else()
  o3os_library(hal DEPENDS core)
endif()
