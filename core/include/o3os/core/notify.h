// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Notifications (ADR 0009 rev 1, OS-002b): a bounded queue of short messages for the user, `{severity, text, source,
// ttl}`. Anything that has to tell the user something (an F-key placeholder, a rejected SD file, a module that fell
// back to its built-in default) posts here; the app draws the head as the notification overlay when no modal overlay
// is up. Pure data and bounded loops: no heap, no clock of its own, usable from any layer.
//
// Time. update() is told the time (a 64-bit millisecond count, hal::Clock::uptime_ms) and whether the head is shown.
// The head's TTL counts down only while it is shown, and only from the update that first showed it: a notification
// posted behind a modal overlay waits with its whole TTL, and one posted now is shown for exactly ttl_ms of updates.
// So a run is a pure function of the posts and the times, which is what the simulator needs.
//
// Coalescing. The same news twice (same text and same source) while the first is still queued is one entry with a count,
// not two: the second post raises the count (shown as "x3"), makes the entry the more severe of the two, and, if it is the
// head, shows it for its whole time again from now. A queue full of other things loses nothing to it.
//
// Progress. News that changes while it is shown (a job's "IMPORT 45%") is one entry that is rewritten, not a queue of
// them: upsert() replaces the entry of the same source in place, and posts a new one if there is none.
//
// Questions. A question is a strip text whose answer is the next key press, and it counts only while it is the head (app/key_prompt.h), so ask() puts it ahead of any Info news (a job's "SAVE DONE"): that
// news is stale by the time the question has run out, so the Info head it displaces is dropped, not shown again after. Warnings and errors keep their places in front of it.
//
// Overflow. The queue holds kCapacity entries. When it is full, the oldest entry of the lowest severity present goes
// to make room, unless it is more severe than the new one (then the new one is refused): an info is dropped before a
// warning before an error, and an error is never dropped for an info (or a warning). The head may be the one dropped.
//
// Main loop only; not thread-safe.

#include <o3os/core/fixed_string.h>

#include <cstddef>
#include <cstdint>

namespace o3os::core {

inline constexpr std::size_t kNotifyTextChars = 48;   // what the user reads: one Label line
inline constexpr std::size_t kNotifySourceChars = 8;  // who posted it
inline constexpr unsigned kNotifyMaxCount = 99;        // the count of a coalesced entry stops here: two digits

enum class Severity : std::uint8_t { Info = 0, Warn = 1, Error = 2 };

struct Notification {
  Severity severity = Severity::Info;
  FixedString<kNotifyTextChars> text;     // longer text is cut
  FixedString<kNotifySourceChars> source;  // "ui", "sd", ...: for the log and the diagnostics, shown dim
  std::uint32_t ttl_ms = 0;  // how long it is shown (at least 1)
  std::uint8_t count = 1;    // how many identical posts it stands for (1..kNotifyMaxCount)
};

class NotifyQueue {
 public:
  static constexpr unsigned kCapacity = 4;

  unsigned size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  // Precondition: i < size(). 0 is the head: the one that is, or is next to be, shown.
  const Notification& at(unsigned i) const noexcept { return items_[i]; }
  const Notification& head() const noexcept { return items_[0]; }
  // Milliseconds of the head's TTL still to run (0 when empty).
  std::uint32_t remaining_ms() const noexcept { return size_ == 0 ? 0u : remaining_; }
  // Entries lost to the overflow policy since boot: dropped to make room, or refused.
  std::uint32_t dropped() const noexcept { return dropped_; }
  // OS-HIL (`diag state`): the text of the news posted last (accepted, merged or rewritten; it stays after the entry has expired), and how many posts there have been since boot
  // (a counter that moves for every one, so a host that polls can tell a repeat from the same old line). "" before any.
  const char* last_text() const noexcept { return last_.c_str(); }
  std::uint32_t posts() const noexcept { return posts_; }

  // Queues a notification (text and source are cut to fit; compared as cut). True: stored, or merged into the identical entry
  // that is already queued (see Coalescing); an older entry may have been dropped for it, dropped() says. False: the queue
  // was full of more severe entries and this one was refused.
  bool post(Severity severity, const char* text, const char* source, std::uint32_t ttl_ms) noexcept;

  // Like post(), for news that changes while it is shown: the oldest queued entry of the same `source` is rewritten in
  // place (severity, text, ttl_ms, count 1) and keeps its place in the queue; if it is the head its whole TTL starts again from
  // now, so a stream of updates keeps it on screen. With no entry of that source it is post(). Same result as post().
  // An empty source never matches (it would replace unrelated news).
  bool upsert(Severity severity, const char* text, const char* source, std::uint32_t ttl_ms) noexcept;

  // upsert() for a question (OS-BUG-PROMPTQ): the entry is stored as upsert() stores it, then moved ahead of every Info entry that stands before it (warnings and errors stay in front of it). When that makes it the
  // head, the Info head it displaced is dropped (not counted in dropped(): it is not an overflow) and the question starts with its whole TTL when it is first shown. Same result as upsert().
  bool ask(Severity severity, const char* text, const char* source, std::uint32_t ttl_ms) noexcept;

  // Lets time run to `now_ms`. While `visible` (the head is on screen) the head's TTL counts down and expired entries
  // leave the queue, each next head starting with its whole TTL at the moment the one before it ran out. Returns true
  // when the head changed (the screen needs a redraw). Call it once a pass, with `visible` as drawn; a gap in calls is
  // counted if the head was shown at the call before it, a hidden head never counts.
  bool update(std::uint64_t now_ms, bool visible) noexcept;

  void clear() noexcept;

  // Takes every queued entry of `source` off (news that is over before its time runs out: OS-METRO's count-in on a stop). An empty source matches nothing. The next entry, if it becomes the head, starts
  // with its whole TTL when it is first shown.
  void dismiss(const char* source) noexcept;
  // Only the entries of `source` whose text is exactly `text` (a question that was taken back: OS-NEWKEEP2); an empty source matches nothing.
  void dismiss(const char* source, const char* text) noexcept;
  // Takes every queued entry of `severity` off, whatever its source (OS-MIDIREC S2b: when the EVENTS list opens, the Info news queued before it goes — the strip waits behind the modal overlay and a hidden head never expires, so a queued Info would mask the list's own refusals after BACK; warnings and errors stay). The next entry, if it becomes the head, starts with its whole TTL when it is first shown.
  void dismiss(Severity severity) noexcept;

 private:
  void remove(unsigned index) noexcept;
  void note(const FixedString<kNotifyTextChars>& text) noexcept {
    last_ = text;
    ++posts_;
  }

  Notification items_[kCapacity];
  unsigned size_ = 0;
  std::uint32_t remaining_ = 0;  // the head's TTL left
  std::uint64_t last_ms_ = 0;    // the time of the last update()
  bool counting_ = false;        // the head was shown at the last update(): the time since then counts
  std::uint32_t dropped_ = 0;
  FixedString<kNotifyTextChars> last_;
  std::uint32_t posts_ = 0;
};

}  // namespace o3os::core
