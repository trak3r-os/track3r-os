// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The panes of the Song screen's STAGE and TIMELINE pages (OS-SONG-VIEW). Private to the Song screen (song_screen.cpp): the strip, header, info line and key row are the screen's own and
// drawn for every page; these draw what is between the header and the info line, `f.pane`.

#include <o3os/app/frame.h>
#include <o3os/app/model.h>
#include <o3os/app/view_state.h>
#include <o3os/ui/renderer.h>

namespace o3os::app {

// OS-SONG-TIMELINE: the visible slots as blocks as wide as they have bars, a colour and the number of the pattern on each, a lane of note-density thumbnails a track, the playing slot and playhead, the cursor.
void draw_song_timeline(ui::Renderer& r, const Frame& f, const Model& model, const ViewState& view, ui::Rect covered) noexcept;

// OS-SONG-STAGE: the playing slot and the next in Display-face type, the bars to the next, a progress bar. The page for live use and low vision.
void draw_song_stage(ui::Renderer& r, const Frame& f, const Model& model, const ViewState& view, ui::Rect covered) noexcept;

}  // namespace o3os::app
