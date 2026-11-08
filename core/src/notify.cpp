// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/notify.h>

namespace o3os::core {

void NotifyQueue::remove(unsigned index) noexcept {
  for (unsigned i = index + 1; i < size_; ++i) {
    items_[i - 1] = items_[i];
  }
  --size_;
}

bool NotifyQueue::post(Severity severity, const char* text, const char* source, std::uint32_t ttl_ms) noexcept {
  Notification fresh;
  fresh.severity = severity;
  fresh.text.assign(text != nullptr ? text : "");
  fresh.source.assign(source != nullptr ? source : "");
  fresh.ttl_ms = ttl_ms != 0 ? ttl_ms : 1u;

  // The same news again: one entry with a count (before anything is dropped for room: it needs none).
  for (unsigned i = 0; i < size_; ++i) {
    Notification& n = items_[i];
    if (n.text == fresh.text.c_str() && n.source == fresh.source.c_str()) {
      n.count = static_cast<std::uint8_t>(n.count < kNotifyMaxCount ? n.count + 1u : kNotifyMaxCount);
      n.severity = fresh.severity > n.severity ? fresh.severity : n.severity;
      n.ttl_ms = fresh.ttl_ms > n.ttl_ms ? fresh.ttl_ms : n.ttl_ms;
      if (i == 0) {
        remaining_ = n.ttl_ms;  // the head again: its whole time from now
      }
      note(fresh.text);
      return true;
    }
  }

  if (size_ == kCapacity) {
    // The oldest entry of the lowest severity is the one to lose.
    unsigned victim = 0;
    for (unsigned i = 1; i < size_; ++i) {
      if (items_[i].severity < items_[victim].severity) {
        victim = i;
      }
    }
    if (items_[victim].severity > severity) {
      ++dropped_;  // everything queued is more severe than this: it is the one refused
      return false;
    }
    remove(victim);
    ++dropped_;
    if (victim == 0) {
      counting_ = false;  // a new head: its TTL starts when it is first shown
      remaining_ = size_ != 0 ? items_[0].ttl_ms : 0u;
    }
  }
  items_[size_] = fresh;
  if (size_ == 0) {
    remaining_ = fresh.ttl_ms;
    counting_ = false;
  }
  ++size_;
  note(fresh.text);
  return true;
}

bool NotifyQueue::upsert(Severity severity, const char* text, const char* source, std::uint32_t ttl_ms) noexcept {
  Notification fresh;
  fresh.severity = severity;
  fresh.text.assign(text != nullptr ? text : "");
  fresh.source.assign(source != nullptr ? source : "");
  fresh.ttl_ms = ttl_ms != 0 ? ttl_ms : 1u;
  if (!fresh.source.empty()) {
    for (unsigned i = 0; i < size_; ++i) {
      if (items_[i].source == fresh.source.c_str()) {
        items_[i] = fresh;
        if (i == 0) {
          remaining_ = fresh.ttl_ms;  // the head again: its whole time from now (counting_ is unchanged)
        }
        note(fresh.text);
        return true;
      }
    }
  }
  return post(severity, text, source, ttl_ms);
}

bool NotifyQueue::ask(Severity severity, const char* text, const char* source, std::uint32_t ttl_ms) noexcept {
  if (!upsert(severity, text, source, ttl_ms)) {
    return false;
  }
  FixedString<kNotifyTextChars> cut;  // (found again as stored: cut to fit)
  FixedString<kNotifySourceChars> from;
  cut.assign(text != nullptr ? text : "");
  from.assign(source != nullptr ? source : "");
  unsigned at = 0;
  while (at < size_ && !(items_[at].text == cut.c_str() && items_[at].source == from.c_str())) {
    ++at;
  }
  if (at >= size_) {
    return true;
  }
  unsigned to = at;  // the place the question takes: in front of the run of Info news right before it, never past a Warn or an Error
  while (to > 0 && items_[to - 1].severity == Severity::Info) {
    --to;
  }
  if (to == at) {
    return true;  // nothing Info in front of it
  }
  const Notification question = items_[at];
  for (unsigned i = at; i > to; --i) {
    items_[i] = items_[i - 1];
  }
  items_[to] = question;
  if (to == 0) {
    remove(1);  // the Info head it displaced: stale by the time the question is over
    counting_ = false;  // a new head: its TTL starts when it is first shown
    remaining_ = items_[0].ttl_ms;
  }
  return true;
}

bool NotifyQueue::update(std::uint64_t now_ms, bool visible) noexcept {
  if (now_ms < last_ms_) {
    now_ms = last_ms_;  // time does not run backwards: an earlier time is no time at all
  }
  const std::uint64_t elapsed = counting_ ? now_ms - last_ms_ : 0u;
  last_ms_ = now_ms;
  bool changed = false;
  if (visible && size_ != 0) {
    std::uint64_t left = elapsed;
    while (size_ != 0 && left >= remaining_) {  // at most kCapacity turns: each one removes an entry
      left -= remaining_;
      remove(0);
      changed = true;
      remaining_ = size_ != 0 ? items_[0].ttl_ms : 0u;
    }
    if (size_ != 0) {
      remaining_ -= static_cast<std::uint32_t>(left);  // left < remaining_ here, so it fits
    }
  }
  counting_ = visible && size_ != 0;
  return changed;
}

void NotifyQueue::dismiss(const char* source) noexcept {
  dismiss(source, nullptr);
}

void NotifyQueue::dismiss(const char* source, const char* text) noexcept {
  if (source == nullptr || *source == '\0') {
    return;
  }
  for (unsigned i = 0; i < size_;) {
    if (items_[i].source != source || (text != nullptr && items_[i].text != text)) {
      ++i;
      continue;
    }
    remove(i);
    if (i == 0) {
      counting_ = false;  // a new head: its TTL starts when it is first shown
      remaining_ = size_ != 0 ? items_[0].ttl_ms : 0u;
    }
  }
}

void NotifyQueue::dismiss(Severity severity) noexcept {
  for (unsigned i = 0; i < size_;) {
    if (items_[i].severity != severity) {
      ++i;
      continue;
    }
    remove(i);
    if (i == 0) {
      counting_ = false;  // a new head: its TTL starts when it is first shown
      remaining_ = size_ != 0 ? items_[0].ttl_ms : 0u;
    }
  }
}

void NotifyQueue::clear() noexcept {
  size_ = 0;
  remaining_ = 0;
  counting_ = false;
}

}  // namespace o3os::core
