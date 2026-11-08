// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/core/notify.h>

#include <cstdint>
#include <string>

using namespace o3os::core;

namespace {

std::string text_of(const NotifyQueue& q, unsigned i) { return q.at(i).text.c_str(); }

// The texts in the queue, head first, as "a b c".
std::string order(const NotifyQueue& q) {
  std::string out;
  for (unsigned i = 0; i < q.size(); ++i) {
    out += (i != 0 ? " " : "") + text_of(q, i);
  }
  return out;
}

}  // namespace

static_assert(NotifyQueue::kCapacity == 4, "the queue holds four entries (OS-002 spec)");
static_assert(kNotifyTextChars == 48, "FixedString<48> text");

TEST_CASE("notify: an entry keeps its fields, long text and source are cut, a TTL of 0 means 1 ms") {
  NotifyQueue q;
  CHECK(q.empty());
  CHECK(q.size() == 0u);
  CHECK(q.remaining_ms() == 0u);
  CHECK(q.post(Severity::Warn, "SD card is full", "sd", 3000));
  CHECK(q.size() == 1u);
  CHECK(q.head().severity == Severity::Warn);
  CHECK(text_of(q, 0) == "SD card is full");
  CHECK(std::string(q.head().source.c_str()) == "sd");
  CHECK(q.head().ttl_ms == 3000u);
  CHECK(q.remaining_ms() == 3000u);

  q.clear();
  CHECK(q.post(Severity::Info, std::string(80, 'x').c_str(), "much too long a source", 0));
  CHECK(q.head().text.size() == kNotifyTextChars);   // cut at 48, still terminated
  CHECK(q.head().source.size() == kNotifySourceChars);
  CHECK(q.head().ttl_ms == 1u);
  q.clear();
  CHECK(q.post(Severity::Info, nullptr, nullptr, 10));  // null text and source are empty strings
  CHECK(text_of(q, 0).empty());
  CHECK(std::string(q.head().source.c_str()).empty());
}

TEST_CASE("notify: the head is shown for exactly its TTL, counted from the update that first showed it") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "t", 2000);
  CHECK_FALSE(q.update(5000, true));  // the first update that shows it starts the clock, whenever the post was
  CHECK(q.remaining_ms() == 2000u);
  CHECK_FALSE(q.update(6999, true));
  CHECK(q.remaining_ms() == 1u);
  CHECK(q.update(7000, true));  // 2000 ms of updates: gone, and the screen must be redrawn
  CHECK(q.empty());
  CHECK_FALSE(q.update(9000, true));  // nothing to expire
}

TEST_CASE("notify: each next head starts with its whole TTL where the one before ended, however long the gap in calls") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "t", 1000);
  q.post(Severity::Info, "b", "t", 1000);
  q.post(Severity::Info, "c", "t", 1000);
  q.update(0, true);
  CHECK(q.update(1500, true));  // a ran out at 1000, b has had 500
  CHECK(order(q) == "b c");
  CHECK(q.remaining_ms() == 500u);
  CHECK(q.update(2500, true));  // b ran out at 2000, c has had 500: one call, one gone
  CHECK(order(q) == "c");
  CHECK(q.remaining_ms() == 500u);

  NotifyQueue big;
  big.post(Severity::Info, "a", "t", 1000);
  big.post(Severity::Info, "b", "t", 1000);
  big.post(Severity::Info, "c", "t", 1000);
  big.update(0, true);
  CHECK(big.update(2999, true));  // two whole TTLs and 999 ms of the third
  CHECK(order(big) == "c");
  CHECK(big.remaining_ms() == 1u);
  CHECK(big.update(3000, true));
  CHECK(big.empty());
  NotifyQueue huge;
  huge.post(Severity::Info, "a", "t", 10);
  huge.post(Severity::Info, "b", "t", 10);
  huge.update(0, true);
  CHECK(huge.update(std::uint64_t{1} << 40, true));  // an hour-scale gap: bounded work, everything gone
  CHECK(huge.empty());
}

TEST_CASE("notify: a hidden head does not count down, and counts again from the update that shows it") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "t", 2000);
  q.update(0, true);
  CHECK_FALSE(q.update(500, true));  // 500 ms shown
  CHECK(q.remaining_ms() == 1500u);
  // A modal overlay is up: the queue waits, however long.
  CHECK_FALSE(q.update(600, false));
  CHECK_FALSE(q.update(60000, false));
  CHECK(q.remaining_ms() == 1500u);  // the time before it was hidden is kept, the time hidden is not counted
  CHECK_FALSE(q.update(60001, true));  // the call that shows it again does not count the gap
  CHECK(q.remaining_ms() == 1500u);
  CHECK_FALSE(q.update(61499, true));
  CHECK(q.update(61501, true));
  CHECK(q.empty());

  // Posted while hidden: waits with the whole TTL.
  NotifyQueue p;
  p.update(0, false);
  p.post(Severity::Info, "late", "t", 1000);
  CHECK_FALSE(p.update(10000, false));
  CHECK(p.remaining_ms() == 1000u);
  CHECK_FALSE(p.update(20000, true));
  CHECK_FALSE(p.update(20999, true));
  CHECK(p.update(21000, true));
}

TEST_CASE("notify: time never runs backwards") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "t", 1000);
  q.update(5000, true);
  CHECK_FALSE(q.update(4000, true));  // an earlier time counts as no time, and does not move the clock back
  CHECK(q.remaining_ms() == 1000u);
  CHECK_FALSE(q.update(5500, true));  // 500 ms on from the 5000 the queue had already seen
  CHECK(q.remaining_ms() == 500u);
}

TEST_CASE("notify: when the queue is full the oldest entry of the lowest severity goes, and an error never goes for an info") {
  NotifyQueue q;
  q.post(Severity::Info, "i1", "t", 100);
  q.post(Severity::Warn, "w1", "t", 100);
  q.post(Severity::Error, "e1", "t", 100);
  q.post(Severity::Info, "i2", "t", 100);
  CHECK(q.size() == 4u);
  CHECK(q.dropped() == 0u);

  CHECK(q.post(Severity::Info, "i3", "t", 100));  // the oldest info goes: it was the head
  CHECK(order(q) == "w1 e1 i2 i3");
  CHECK(q.dropped() == 1u);
  CHECK(q.post(Severity::Error, "e2", "t", 100));  // an error takes an info's place: the oldest info, i2
  CHECK(order(q) == "w1 e1 i3 e2");
  CHECK(q.post(Severity::Info, "i4", "t", 100));   // the only info left is i3
  CHECK(order(q) == "w1 e1 e2 i4");
  CHECK(q.post(Severity::Warn, "w2", "t", 100));   // i4 is still the lowest
  CHECK(order(q) == "w1 e1 e2 w2");
  CHECK_FALSE(q.post(Severity::Info, "i5", "t", 100));  // nothing but warnings and errors: an info is the one refused
  CHECK(order(q) == "w1 e1 e2 w2");
  CHECK(q.dropped() == 5u);
  CHECK(q.post(Severity::Warn, "w3", "t", 100));   // a warning replaces the oldest warning
  CHECK(order(q) == "e1 e2 w2 w3");
  CHECK(q.post(Severity::Error, "e3", "t", 100));  // an error replaces the lowest, w2 (oldest of the warnings)
  CHECK(order(q) == "e1 e2 w3 e3");
  CHECK(q.post(Severity::Error, "e4", "t", 100));
  CHECK(order(q) == "e1 e2 e3 e4");
  CHECK_FALSE(q.post(Severity::Warn, "w4", "t", 100));  // all errors: neither an info nor a warning displaces one
  CHECK_FALSE(q.post(Severity::Info, "i6", "t", 100));
  CHECK(order(q) == "e1 e2 e3 e4");
  CHECK(q.post(Severity::Error, "e5", "t", 100));  // an error against errors: the oldest goes
  CHECK(order(q) == "e2 e3 e4 e5");
}

TEST_CASE("notify: dropping the head for room gives the next one its whole TTL") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "t", 1000);
  q.post(Severity::Error, "b", "t", 3000);
  q.post(Severity::Error, "c", "t", 3000);
  q.post(Severity::Error, "d", "t", 3000);
  q.update(0, true);
  q.update(600, true);
  CHECK(q.remaining_ms() == 400u);
  CHECK(q.post(Severity::Info, "e", "t", 1000));  // a is the oldest info and the head: it is dropped
  CHECK(order(q) == "b c d e");
  CHECK(q.remaining_ms() == 3000u);
  CHECK_FALSE(q.update(700, true));  // the call after counts from where the new head began
  CHECK(q.remaining_ms() == 3000u);
  CHECK_FALSE(q.update(3699, true));
  CHECK(q.update(3700, true));
  CHECK(order(q) == "c d e");
}

TEST_CASE("notify: the same posts and times give the same queue, always") {
  const auto run = [] {
    NotifyQueue q;
    std::string trace;
    std::uint64_t now = 0;
    for (unsigned i = 0; i < 40; ++i) {
      q.post(static_cast<Severity>(i % 3), (std::string("n") + std::to_string(i)).c_str(), "t", 100 + 37 * (i % 7));
      now += 53 + i % 5;
      const bool changed = q.update(now, i % 4 != 3);
      trace += std::to_string(q.size()) + (changed ? "c" : "-") + std::to_string(q.remaining_ms()) + ";";
    }
    return trace;
  };
  CHECK(run() == run());
  CHECK_FALSE(run().empty());
}

TEST_CASE("notify: the same news twice is one entry with a count, in whatever place it is queued") {
  NotifyQueue q;
  q.post(Severity::Info, "F3 JUMP", "ui", 2000);
  CHECK(q.at(0).count == 1);
  CHECK(q.post(Severity::Info, "F3 JUMP", "ui", 2000));
  CHECK(q.size() == 1u);
  CHECK(q.at(0).count == 2);
  q.post(Severity::Info, "F4 FILL", "ui", 2000);  // another text: another entry
  q.post(Severity::Info, "F3 JUMP", "sd", 2000);  // another source: another entry
  CHECK(order(q) == "F3 JUMP F4 FILL F3 JUMP");
  q.post(Severity::Info, "F4 FILL", "ui", 2000);  // not the head: the entry in its place takes the count
  CHECK(order(q) == "F3 JUMP F4 FILL F3 JUMP");
  CHECK(q.at(1).count == 2);
  CHECK(q.at(2).count == 1);
  CHECK(q.dropped() == 0u);

  // Compared as cut: two texts that are one after the 48th character are one.
  NotifyQueue cut;
  cut.post(Severity::Info, (std::string(48, 'x') + "A").c_str(), "t", 100);
  cut.post(Severity::Info, (std::string(48, 'x') + "B").c_str(), "t", 100);
  CHECK(cut.size() == 1u);
  CHECK(cut.at(0).count == 2);
  // ...and so are two sources.
  NotifyQueue src;
  src.post(Severity::Info, "a", "12345678X", 100);
  src.post(Severity::Info, "a", "12345678Y", 100);
  CHECK(src.size() == 1u);

  // The count stops at 99, and null text and sources are the empty ones.
  NotifyQueue many;
  for (unsigned i = 0; i < 150; ++i) {
    many.post(Severity::Info, "again", "t", 100);
  }
  CHECK(many.size() == 1u);
  CHECK(many.at(0).count == kNotifyMaxCount);
  NotifyQueue nulls;
  nulls.post(Severity::Info, nullptr, nullptr, 100);
  nulls.post(Severity::Info, "", "", 100);
  CHECK(nulls.size() == 1u);
  CHECK(nulls.at(0).count == 2);
}

TEST_CASE("notify: a merged entry is the more severe of the two, shows for the longer TTL, and the head starts afresh") {
  NotifyQueue q;
  q.post(Severity::Info, "disk", "sd", 1000);
  q.update(0, true);
  q.update(700, true);
  CHECK(q.remaining_ms() == 300u);
  q.post(Severity::Warn, "disk", "sd", 3000);  // the head again, worse, and for longer
  CHECK(q.size() == 1u);
  CHECK(q.head().severity == Severity::Warn);
  CHECK(q.head().ttl_ms == 3000u);
  CHECK(q.remaining_ms() == 3000u);           // its whole time again from now
  q.post(Severity::Info, "disk", "sd", 500);  // a milder, shorter repeat takes nothing away
  CHECK(q.head().severity == Severity::Warn);
  CHECK(q.head().ttl_ms == 3000u);
  CHECK_FALSE(q.update(700, true));
  CHECK_FALSE(q.update(3699, true));
  CHECK(q.update(3700, true));  // 3000 ms from the last repeat (at 700)
  CHECK(q.empty());
  // Once it has run out, the same news is a new entry with a count of 1.
  q.post(Severity::Info, "disk", "sd", 1000);
  CHECK(q.at(0).count == 1);

  // A repeat of a waiting entry (not the head) changes the head's time not at all.
  NotifyQueue w;
  w.post(Severity::Info, "a", "t", 1000);
  w.post(Severity::Info, "b", "t", 1000);
  w.update(0, true);
  w.update(600, true);
  w.post(Severity::Info, "b", "t", 1000);
  CHECK(w.remaining_ms() == 400u);
  CHECK(w.at(1).count == 2);
}

TEST_CASE("notify: a repeat is merged even when the queue is full, and loses nothing") {
  NotifyQueue q;
  q.post(Severity::Error, "e", "t", 100);
  q.post(Severity::Warn, "w", "t", 100);
  q.post(Severity::Info, "i", "t", 100);
  q.post(Severity::Info, "j", "t", 100);
  CHECK(q.size() == 4u);
  CHECK(q.post(Severity::Info, "i", "t", 100));  // full, but nothing needs to go
  CHECK(order(q) == "e w i j");
  CHECK(q.dropped() == 0u);
  CHECK(q.at(2).count == 2);
  // A repeat of something that is no longer queued is new news: it is subject to the overflow policy as before.
  CHECK(q.post(Severity::Info, "k", "t", 100));
  CHECK(order(q) == "e w j k");
  CHECK(q.dropped() == 1u);
}

TEST_CASE("notify: upsert rewrites the entry of its source in place, and posts when there is none") {
  NotifyQueue q;
  CHECK(q.upsert(Severity::Info, "IMPORT 0%", "job", 1000));  // none yet: a post
  CHECK(q.size() == 1u);
  q.post(Severity::Warn, "w", "sd", 5000);
  q.update(0, true);
  CHECK(q.update(400, true) == false);
  CHECK(q.remaining_ms() == 600u);

  // The head is rewritten and shown for its whole TTL again from now: a stream of updates keeps it up.
  CHECK(q.upsert(Severity::Info, "IMPORT 40%", "job", 1000));
  CHECK(order(q) == "IMPORT 40% w");
  CHECK(q.size() == 2u);  // not queued next to the old one
  CHECK(q.remaining_ms() == 1000u);
  CHECK_FALSE(q.update(1300, true));  // the queue knows the time only from update(): 900 ms since the last call
  CHECK(q.remaining_ms() == 100u);

  // Not the head: rewritten where it stands, nothing else disturbed.
  NotifyQueue p;
  p.post(Severity::Warn, "first", "sd", 5000);
  p.upsert(Severity::Info, "IMPORT 1%", "job", 1000);
  p.post(Severity::Info, "last", "ui", 1000);
  p.update(0, true);
  p.update(1000, true);
  const std::uint32_t head_left = p.remaining_ms();
  CHECK(p.upsert(Severity::Error, "IMPORT FAILED", "job", 3000));
  CHECK(order(p) == "first IMPORT FAILED last");
  CHECK(p.at(1).severity == Severity::Error);
  CHECK(p.at(1).ttl_ms == 3000u);
  CHECK(p.at(1).count == 1u);
  CHECK(p.remaining_ms() == head_left);  // the head's clock is untouched

  // It does not match a different source, or an empty one, and it does not coalesce counts of an old entry.
  NotifyQueue r;
  r.post(Severity::Info, "a", "ui", 1000);
  r.post(Severity::Info, "a", "ui", 1000);  // x2
  CHECK(r.at(0).count == 2u);
  CHECK(r.upsert(Severity::Info, "b", "job", 1000));
  CHECK(r.size() == 2u);
  CHECK(r.upsert(Severity::Info, "c", "", 1000));  // an empty source never replaces anything
  CHECK(r.upsert(Severity::Info, "d", "", 1000));
  CHECK(order(r) == "a b c d");
  CHECK(r.upsert(Severity::Info, "a2", "ui", 1000));  // rewrites the "ui" entry: the count goes back to 1
  CHECK(r.at(0).count == 1u);
  CHECK(order(r) == "a2 b c d");

  // On a full queue with no entry of its source it behaves as post(): the overflow rule decides.
  NotifyQueue f;
  f.post(Severity::Error, "e1", "x", 100);
  f.post(Severity::Error, "e2", "y", 100);
  f.post(Severity::Error, "e3", "z", 100);
  f.post(Severity::Error, "e4", "w", 100);
  CHECK_FALSE(f.upsert(Severity::Info, "progress", "job", 100));  // refused: everything queued is more severe
  CHECK(f.upsert(Severity::Error, "e2b", "y", 100));              // replaced in place, no room needed
  CHECK(order(f) == "e1 e2b e3 e4");
}

TEST_CASE("notify: the last text and the post counter survive the entry (OS-HIL, `diag state`)") {
  NotifyQueue q;
  CHECK(std::string(q.last_text()) == "");
  CHECK(q.posts() == 0u);
  CHECK(q.post(Severity::Info, "SAVE DONE", "save", 100));
  CHECK(std::string(q.last_text()) == "SAVE DONE");
  CHECK(q.posts() == 1u);
  CHECK(q.post(Severity::Info, "SAVE DONE", "save", 100));  // the same news: coalesced, but a post all the same
  CHECK(q.size() == 1u);
  CHECK(q.posts() == 2u);
  CHECK(q.upsert(Severity::Info, "IMPORT 45%", "imp", 100));  // a new entry
  CHECK(q.upsert(Severity::Info, "IMPORT 90%", "imp", 100));  // rewritten in place
  CHECK(std::string(q.last_text()) == "IMPORT 90%");
  CHECK(q.posts() == 4u);
  // the entries run out; the last news stays
  CHECK(q.update(1000, true) == false);
  CHECK(q.update(5000, true));
  CHECK(q.empty());
  CHECK(std::string(q.last_text()) == "IMPORT 90%");
  CHECK(q.posts() == 4u);
  // a refused post (the queue is full of more severe news) is not news
  for (unsigned i = 0; i < NotifyQueue::kCapacity; ++i) {
    CHECK(q.post(Severity::Error, ("E" + std::to_string(i)).c_str(), "e", 1000));
  }
  const std::uint32_t posts = q.posts();
  CHECK_FALSE(q.post(Severity::Info, "LOST", "x", 1000));
  CHECK(q.posts() == posts);
  CHECK(std::string(q.last_text()) == "E3");
  q.clear();
  CHECK(std::string(q.last_text()) == "E3");  // clear() empties the queue, not the history
}

// OS-METRO: the count-in's text is taken off the moment the count-in ends.
TEST_CASE("notify: dismiss takes every entry of a source off, a new head starts with its whole TTL, and an empty or unknown source touches nothing") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "cntin", 500);
  q.post(Severity::Warn, "b", "sd", 3000);
  q.post(Severity::Info, "c", "cntin", 700);
  CHECK_FALSE(q.update(1000, true));  // "a" is shown from here
  CHECK_FALSE(q.update(1200, true));
  CHECK(q.remaining_ms() == 300u);
  q.dismiss("");
  q.dismiss(nullptr);
  q.dismiss("nobody");
  CHECK(order(q) == "a b c");
  q.dismiss("cntin");  // the head and the entry behind the other one
  CHECK(order(q) == "b");
  CHECK(q.remaining_ms() == 3000u);  // "b" has not been shown yet: its whole time from the update that shows it
  CHECK_FALSE(q.update(1300, true));
  CHECK(q.remaining_ms() == 3000u);
  CHECK_FALSE(q.update(2300, true));
  CHECK(q.remaining_ms() == 2000u);
  q.dismiss("sd");
  CHECK(q.empty());
  CHECK(q.remaining_ms() == 0u);
  CHECK_FALSE(q.update(9000, true));
}

// OS-NEWKEEP2: a question that was taken back is dismissed by its text, whatever its place in the queue.
TEST_CASE("notify: dismiss with a text takes only the entries of that source with exactly that text off") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "file", 500);
  q.post(Severity::Warn, "ask", "sd", 3000);
  q.post(Severity::Warn, "ask", "file", 3000);
  q.post(Severity::Info, "ask 2", "file", 700);
  q.dismiss("", "ask");
  q.dismiss("file", "nothing");
  q.dismiss("sd", "a");
  CHECK(order(q) == "a ask ask ask 2");
  q.dismiss("file", "ask");  // not the same text of another source, nor a longer text
  CHECK(order(q) == "a ask ask 2");
  q.dismiss("file", "a");  // the head: the next one starts with its whole time
  CHECK(order(q) == "ask ask 2");
  CHECK(q.remaining_ms() == 3000u);
}

// OS-MIDIREC S2b: opening the EVENTS list takes every Info entry off, whatever its source (a hidden head never expires behind the modal overlay, so a queued Info would mask the list's own refusals after BACK); warnings and errors stay.
TEST_CASE("notify: dismiss with a severity takes every entry of that severity off, whatever its source, and keeps the more severe ones") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "history", 500);
  q.post(Severity::Warn, "b", "history", 3000);
  q.post(Severity::Info, "c", "edit", 700);
  q.post(Severity::Error, "d", "sd", 900);
  CHECK(order(q) == "a b c d");
  q.dismiss(Severity::Info);
  CHECK(order(q) == "b d");  // "b" was not the head: its whole TTL is still there
  CHECK(q.remaining_ms() == 3000u);
  q.post(Severity::Info, "e", "ui", 100);
  CHECK(order(q) == "b d e");
  q.dismiss(Severity::Warn);
  CHECK(order(q) == "d e");
  CHECK(q.remaining_ms() == 900u);  // "b" was the head: "d" starts with its whole time
  q.dismiss(Severity::Error);  // the head: the next one starts with its whole time
  CHECK(order(q) == "e");
  CHECK(q.remaining_ms() == 100u);
  q.dismiss(Severity::Info);
  CHECK(q.empty());
  CHECK(q.remaining_ms() == 0u);
}

// OS-BUG-PROMPTQ: a question counts only as the head of the strip, so it goes ahead of Info news (a job's DONE) and the Info head it displaces is dropped; warnings and errors keep their places.
TEST_CASE("notify: ask puts a question ahead of Info news and drops the Info head it displaces") {
  NotifyQueue q;
  q.post(Severity::Info, "SAVE DONE", "job", 2000);
  CHECK_FALSE(q.update(1000, true));
  CHECK_FALSE(q.update(1500, true));
  CHECK(q.remaining_ms() == 1500u);
  CHECK(q.ask(Severity::Warn, "NEW?", "file", 4000));
  CHECK(order(q) == "NEW?");  // the DONE is gone, not queued behind
  CHECK(q.remaining_ms() == 4000u);
  CHECK(q.dropped() == 0u);  // (not an overflow)
  CHECK(std::string(q.last_text()) == "NEW?");
  CHECK_FALSE(q.update(1600, true));  // the question starts with its whole time from the update that shows it
  CHECK_FALSE(q.update(2600, true));
  CHECK(q.remaining_ms() == 3000u);

  // The same question again (the same source): rewritten, still the head.
  CHECK(q.ask(Severity::Warn, "NEW? 2", "file", 4000));
  CHECK(order(q) == "NEW? 2");
  CHECK(q.remaining_ms() == 4000u);
}

TEST_CASE("notify: ask keeps warnings and errors in front of the question and Info news behind it") {
  NotifyQueue q;
  q.post(Severity::Error, "E", "sd", 3000);
  q.post(Severity::Info, "i1", "a", 1000);
  q.post(Severity::Info, "i2", "b", 1000);
  CHECK(q.ask(Severity::Warn, "Q", "file", 4000));
  CHECK(order(q) == "E Q i1 i2");  // the error is shown first, the unseen Info news waits behind the question
  CHECK(q.remaining_ms() == 3000u);

  // An Info question (the page reset's) behind Info news takes the head.
  NotifyQueue r;
  r.post(Severity::Info, "a", "x", 1000);
  r.post(Severity::Info, "b", "y", 1000);
  CHECK(r.ask(Severity::Info, "Q", "defaults", 2000));
  CHECK(order(r) == "Q b");  // "a" was the head: dropped
  CHECK(r.remaining_ms() == 2000u);

  // Already in front of all the Info news: nothing moves.
  NotifyQueue s;
  CHECK(s.ask(Severity::Warn, "Q", "file", 4000));
  CHECK(order(s) == "Q");
  s.post(Severity::Info, "i", "job", 1000);
  CHECK(s.ask(Severity::Warn, "Q", "file", 4000));  // (the same source: rewritten in place)
  CHECK(order(s) == "Q i");
}

TEST_CASE("notify: ask never jumps a warning or an error, even with Info news in front of it") {
  NotifyQueue q;
  q.post(Severity::Info, "i", "a", 1000);
  q.post(Severity::Warn, "w", "b", 1000);
  CHECK(q.ask(Severity::Warn, "Q", "file", 4000));
  CHECK(order(q) == "i w Q");  // [Info, Warn, Q]: the question stays behind the warning, and the Info head is not dropped
  CHECK(q.remaining_ms() == 1000u);

  NotifyQueue r;
  r.post(Severity::Info, "i", "a", 1000);
  r.post(Severity::Error, "e", "b", 1000);
  CHECK(r.ask(Severity::Warn, "Q", "file", 4000));
  CHECK(order(r) == "i e Q");

  // Only the run of Info news right in front of the question is passed.
  NotifyQueue t;
  t.post(Severity::Warn, "w", "a", 1000);
  t.post(Severity::Info, "i1", "b", 1000);
  t.post(Severity::Info, "i2", "c", 1000);
  CHECK(t.ask(Severity::Warn, "Q", "file", 4000));
  CHECK(order(t) == "w Q i1 i2");
}

TEST_CASE("notify: ask on a full queue makes room as post does, and is refused when everything queued is more severe") {
  NotifyQueue q;
  q.post(Severity::Info, "a", "w", 1000);
  q.post(Severity::Info, "b", "x", 1000);
  q.post(Severity::Info, "c", "y", 1000);
  q.post(Severity::Info, "d", "z", 1000);
  CHECK(q.ask(Severity::Warn, "Q", "file", 4000));  // "a" goes for room
  CHECK(order(q) == "Q c d");  // (the displaced head "b" is dropped too)
  NotifyQueue e;
  e.post(Severity::Error, "e1", "a", 100);
  e.post(Severity::Error, "e2", "b", 100);
  e.post(Severity::Error, "e3", "c", 100);
  e.post(Severity::Error, "e4", "d", 100);
  CHECK_FALSE(e.ask(Severity::Warn, "Q", "file", 4000));
  CHECK(order(e) == "e1 e2 e3 e4");
}
