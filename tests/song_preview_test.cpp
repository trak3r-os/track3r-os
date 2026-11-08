// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG3 S4: the state of PREVIEW (F1 in a TIMELINE lane). The rectangle's tracks become the solo set; the set from before comes back when the audition ends, once; a begin while active keeps the first
// saved set; the end of an audition that never began restores nothing (no stuck solo).
#include <doctest/doctest.h>

#include <o3os/app/song_preview.h>

using o3os::app::SongPreview;

TEST_CASE("song_preview: the mask of a rectangle of tracks") {
  CHECK(SongPreview::mask_of(0, 0) == 0x0001);
  CHECK(SongPreview::mask_of(2, 4) == 0x001C);
  CHECK(SongPreview::mask_of(4, 2) == 0x001C);  // the anchor can be past the cursor
  CHECK(SongPreview::mask_of(14, 20) == 0xC000);  // tracks past 15 are dropped
  CHECK(SongPreview::mask_of(-3, 1) == 0x0003);
}

TEST_CASE("song_preview: begin applies the rectangle, end restores the state from before") {
  SongPreview p;
  CHECK_FALSE(p.active());
  CHECK(p.begin(0x0021, 1, 2) == 0x0006);  // tracks 0 and 5 were soloed by the player
  CHECK(p.active());
  bool restore = false;
  CHECK(p.end(restore) == 0x0021);
  CHECK(restore);
  CHECK_FALSE(p.active());
}

TEST_CASE("song_preview: a second end is a no-op, an end with no audition restores nothing") {
  SongPreview p;
  bool restore = true;
  (void)p.end(restore);
  CHECK_FALSE(restore);
  (void)p.begin(0, 3, 3);
  (void)p.end(restore);
  CHECK(restore);
  (void)p.end(restore);
  CHECK_FALSE(restore);
}

TEST_CASE("song_preview: a begin after an end starts afresh from the set given") {
  SongPreview p;
  (void)p.begin(0x0008, 0, 1);
  bool restore = false;
  CHECK(p.end(restore) == 0x0008);
  CHECK(restore);
  CHECK(p.begin(0x0003, 4, 4) == 0x0010);  // a new audition saves the set in force now
  CHECK(p.saved() == 0x0003);
  CHECK(p.end(restore) == 0x0003);
  CHECK(restore);
}
