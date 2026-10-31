// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pane of the Song screen's TIMELINE page (OS-SONG-VIEW). Private to the Song screen (song_screen.cpp): the strip, header, info line and key row are the screen's own and
// drawn for every page; these draw what is between the header and the info line, `f.pane`.

#include <o3os/app/frame.h>
#include <o3os/app/model.h>
#include <o3os/app/view_state.h>
#include <o3os/ui/renderer.h>

namespace o3os::app {

// OS-SONG-TIMELINE: the visible slots as blocks as wide as they have bars, a colour and the number of the pattern on each, a note map (a lane of ticks a track, song_notemap.h), the energy row, the playing slot and playhead, the cursor.
void draw_song_timeline(ui::Renderer& r, const Frame& f, const Model& model, const ViewState& view, ui::Rect covered) noexcept;

}  // namespace o3os::app
