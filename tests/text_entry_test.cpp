// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FU2: the on-screen keyboard as a state machine (app/text_entry.h): the key tables and the
// pad map, a typed key over the selection, the limit, backspace, caps, the name as a focus of its own with the edit cursor; the wrapping focus (PAR-116, rules documented as OS-new);
// the file name filter.
#include <doctest/doctest.h>

#include <o3os/app/module_ui.h>  // kPresetNameMax (OS-MODPRESET P3)
#include <o3os/app/text_entry.h>

#include <cstring>
#include <string>

using namespace o3os::app;

TEST_CASE("text entry: the key tables are 42 keys in rows of 11, 12, 11 and 8, and every pad types the key the pad map names") {
  CHECK(std::string(kTextKeysSmall + 11, 12).substr(0, 10) == "qwertyuiop");
  CHECK(kTextKeysSmall[kTextKeyBackspace] == 0);
  CHECK(kTextKeysSmall[kTextKeyCaps] == 1);
  CHECK(kTextKeysSmall[kTextKeySpace] == ' ');
  CHECK(kTextKeysBig[11] == 'Q');
  for (unsigned k = 0; k < kTextKeys; ++k) {
    const char s = kTextKeysSmall[k];
    const char b = kTextKeysBig[k];
    const bool same_or_upper = s == b || (s >= 'a' && s <= 'z' && b == s - 32);
    CHECK(same_or_upper);
  }
  CHECK(kTextRowStart[1] - kTextRowStart[0] == 11);
  CHECK(kTextRowStart[2] - kTextRowStart[1] == 12);
  CHECK(kTextRowStart[3] - kTextRowStart[2] == 11);
  CHECK(kTextRowStart[4] - kTextRowStart[3] == 8);
  // pads: 12 a row; the backspace and caps keys two pads wide, space five
  CHECK(kTextPadKey[10] == kTextKeyBackspace);
  CHECK(kTextPadKey[11] == kTextKeyBackspace);
  CHECK(kTextPadKey[34] == kTextKeyCaps);
  CHECK(kTextPadKey[35] == kTextKeyCaps);
  for (unsigned p = 43; p < 48; ++p) {
    CHECK(kTextPadKey[p] == kTextKeySpace);
  }
  CHECK(kTextPadKey[12] == 11);  // 'q'
  CHECK(kTextPadKey[36] == 34);  // 'z'
  TextEntry t;
  t.begin("", 32, TextEntry::Filter::Any);
  CHECK(t.pad(12));  // q
  CHECK(t.pad(36));  // z
  CHECK(t.pad(45));  // space
  CHECK(std::string(t.text()) == "qz ");
}

TEST_CASE("text entry: a key typed over the selected name replaces all of it, a backspace deletes all of it; typing then goes on at the edit cursor") {
  TextEntry t;
  t.begin("Track 01", 32, TextEntry::Filter::Any);
  CHECK(t.selected());
  CHECK(t.length() == 8u);
  CHECK(t.type(11));  // q
  CHECK(std::string(t.text()) == "q");
  CHECK_FALSE(t.selected());
  CHECK(t.type(12));
  CHECK(std::string(t.text()) == "qw");
  t.set("Hello");
  CHECK(t.selected());
  CHECK(t.backspace());
  CHECK(std::string(t.text()) == "");
  CHECK_FALSE(t.backspace());  // nothing to delete
  t.set("abc");
  CHECK(t.move(0, -1));  // up from the keys (the focus is on a letter of the second row): the first row
  CHECK_FALSE(t.frame());
  CHECK(t.move(0, -1));  // and up from there: the name
  CHECK(t.frame());
  CHECK(t.length() == 3u);
  CHECK(t.move(-1, 0));
  CHECK(t.edit() == 2u);
  CHECK(t.move(-1, 0));
  CHECK(t.move(-1, 0));
  CHECK(t.edit() == 0u);
  CHECK(t.move(1, 0));
  CHECK(t.edit() == 1u);
  CHECK(t.move(0, 1));  // down: back to the first key
  CHECK_FALSE(t.frame());
  CHECK(t.key() == 0u);
  CHECK(t.type(11));  // a q at the edit cursor, after the "a"
  CHECK(std::string(t.text()) == "aqbc");
  CHECK(t.backspace());  // takes the q back
  CHECK(std::string(t.text()) == "abc");
}

TEST_CASE("text entry: typing stops at the limit, caps gives the big letters and toggles back, the focus key is typed by confirm_key") {
  TextEntry t;
  t.begin("", 5, TextEntry::Filter::Any);
  for (int i = 0; i < 9; ++i) {
    (void)t.type(0);  // '1'
  }
  CHECK(t.length() == 5u);
  CHECK_FALSE(t.type(0));
  CHECK(std::string(t.text()) == "11111");
  t.set("");
  CHECK(t.type(kTextKeyCaps));
  CHECK(t.shift());
  CHECK(t.type(11));
  CHECK(t.type(kTextKeyCaps));
  CHECK_FALSE(t.shift());
  CHECK(t.type(11));
  CHECK(std::string(t.text()) == "Qq");
  (void)t.type(12);
  t.set("");
  (void)t.move(1, 0);  // the focus is on 'w' (key 12) after type(12): moves on to 'e'
  CHECK(t.key() == 13u);
  CHECK(t.confirm_key());
  CHECK(std::string(t.text()) == "e");
  // the capacity of the field here, 32, is the most a text takes (the official firmware's limit is an open research question)
  TextEntry u;
  u.begin("0123456789012345678901234567890123456789", 99, TextEntry::Filter::Any);
  CHECK(u.length() == 32u);
  CHECK(u.capacity() == 32u);
}

TEST_CASE("text entry: the focus wraps inside a row, goes up and down between rows keeping the column, and round through the name") {
  TextEntry t;
  t.begin("x", 32, TextEntry::Filter::Any);
  CHECK(t.key() == kTextKeyBackspace);  // it starts on the backspace key (the end of the first row)
  CHECK(t.move(1, 0));
  CHECK(t.key() == 0u);  // wrapped to the row's first key
  CHECK(t.move(-1, 0));
  CHECK(t.key() == 10u);
  CHECK(t.move(0, 1));  // down: the row of 12, column 10
  CHECK(t.key() == 21u);
  CHECK(t.move(1, 0));
  CHECK(t.key() == 22u);
  CHECK(t.move(1, 0));
  CHECK(t.key() == 11u);  // wrapped
  CHECK(t.move(-1, 0));
  CHECK(t.key() == 22u);
  CHECK(t.move(0, 1));  // third row has 11: column 11 is held to 10
  CHECK(t.key() == 33u);
  CHECK(t.move(0, 1));  // fourth row has 8: column 10 is held to 7
  CHECK(t.key() == 41u);
  CHECK(t.move(0, 1));  // off the bottom: the name
  CHECK(t.frame());
  CHECK(t.move(0, 1));  // and down from the name: the first key
  CHECK(t.key() == 0u);
  CHECK(t.move(0, -1));  // up from the top row: the name
  CHECK(t.frame());
  CHECK_FALSE(t.move(0, -1));
  CHECK_FALSE(t.confirm_key());  // nothing is typed while the focus is in the name
}

TEST_CASE("text entry: a name for a file refuses the keys FAT and the save layer refuse, a free text takes them") {
  TextEntry f;
  f.begin("", 31, TextEntry::Filter::FileName);
  CHECK_FALSE(f.type(22));  // '+'
  CHECK_FALSE(f.type(32));  // '@'
  CHECK(f.type(21));        // '-'
  CHECK(f.type(0));         // '1'
  CHECK(f.type(kTextKeySpace));
  CHECK(f.type(kTextKeyCaps));  // caps is always allowed
  CHECK(f.type(11));
  CHECK(std::string(f.text()) == "-1 Q");
  CHECK_FALSE(f.key_allowed(22));
  CHECK(f.key_allowed(21));
  TextEntry a;
  a.begin("", 32, TextEntry::Filter::Any);
  CHECK(a.type(22));
  CHECK(a.type(32));
  CHECK(std::string(a.text()) == "+@");
}

TEST_CASE("text entry: a preset's name (OS-MODPRESET P4, Target::Preset) is a FileName-filtered entry capped at kPresetNameMax, selected whole when it opens") {
  TextEntry e;
  e.begin("REVERB", kPresetNameMax, TextEntry::Filter::FileName);
  CHECK(e.capacity() == kPresetNameMax);
  CHECK(std::string(e.text()) == "REVERB");
  CHECK(e.selected());  // typed over, the suggestion goes
  CHECK(e.type(0));     // '1' replaces all of it
  CHECK(std::string(e.text()) == "1");
  CHECK_FALSE(e.selected());
  // The cap holds: 30 characters, the guard's kPresetNameMax.
  TextEntry long_name;
  long_name.begin("", kPresetNameMax, TextEntry::Filter::FileName);
  for (unsigned i = 0; i < 40u; ++i) {
    (void)long_name.type(11);  // 'q'
  }
  CHECK(long_name.length() == kPresetNameMax);
  CHECK_FALSE(long_name.type(11));
}
