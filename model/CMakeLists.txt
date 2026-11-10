# SPDX-License-Identifier: GPL-3.0-or-later
# model: the project model (patterns, song, instruments, tracks, modules) and its mutation API, no heap, no
# knowledge of screens, files or devices. Depends on core. OS-004a. The undo journal and the Editor over the Project (OS-002c); the
# parameter tables and the settings block (OS-002d); the demo content a device starts with (OS-005d lite).
o3os_library(model SOURCES src/project.cpp src/instrument.cpp src/undo.cpp src/editor.cpp src/params.cpp src/settings.cpp
                    src/step_edit.cpp src/pattern_fill.cpp src/pattern_verbs.cpp src/demo.cpp src/project_state.cpp src/instrument_use.cpp src/event_list.cpp src/lane_store.cpp DEPENDS core)
