#pragma once
// Date headings in the Text and Enigma chat histories (presentation only:
// nothing here is stored, counted or selectable). A heading precedes the
// first shown message of each calendar day in Thailand time (UTC+7), taken
// from the message's stored effective_sort_timestamp -- sent and received
// messages alike, in the history's own order. A timestamp that cannot be
// trusted (the clock was not set: 0, or anything before 2020 or from 2100 on)
// gets an "Unknown date" heading instead of an invented date; consecutive
// such messages share it. Thai glyphs are not in the display font
// (FreeSans9pt7b, ASCII), so the date is shown as dd/mm/yyyy.
#include <stdint.h>
#include <stdio.h>

#include "core/storage_messages.h"

namespace HistoryDates {

constexpr uint32_t kUnknownDay = 0xFFFFFFFFu;
constexpr uint32_t kTrustedFrom = 1577836800u;   // 2020-01-01T00:00:00Z
constexpr uint32_t kTrustedUntil = 4102444800u;  // 2100-01-01T00:00:00Z
constexpr uint32_t kThaiOffsetSeconds = 7u * 3600u;
constexpr size_t kTextCap = 16;

// Days since 1970-01-01 in Thailand time, or kUnknownDay.
inline uint32_t thaiDay(uint32_t ts) {
  if (ts < kTrustedFrom || ts >= kTrustedUntil) return kUnknownDay;
  return (ts + kThaiOffsetSeconds) / 86400u;
}

// Whether history entry `index` (the open chat's conversation index) is the
// first of its day: the first entry, or a different day from the one before.
inline bool startsDay(uint16_t index) {
  const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(index);
  if (e == nullptr) return false;
  if (index == 0) return true;
  const MessageStore::ConversationIndexEntry* p = MessageStore::getIndexEntry(static_cast<uint16_t>(index - 1));
  return p == nullptr || thaiDay(p->effective_sort_timestamp) != thaiDay(e->effective_sort_timestamp);
}

// "dd/mm/yyyy" for a day from thaiDay() (civil-from-days, proleptic
// Gregorian), or "Unknown date".
inline void formatDay(uint32_t day, char* out, size_t cap) {
  if (day == kUnknownDay) {
    snprintf(out, cap, "Unknown date");
    return;
  }
  int32_t z = static_cast<int32_t>(day) + 719468;
  int32_t era = z / 146097;
  uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = static_cast<int32_t>(yoe) + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  // (narrow types: d 1..31, m 1..12, y 2019..2100 for a trusted day)
  snprintf(out, cap, "%02u/%02u/%04u", static_cast<unsigned>(static_cast<uint8_t>(d)), static_cast<unsigned>(static_cast<uint8_t>(m)),
           static_cast<unsigned>(static_cast<uint16_t>(y)));
}

// The heading text for history entry `index`.
inline void headingFor(uint16_t index, char* out, size_t cap) {
  const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(index);
  formatDay(e != nullptr ? thaiDay(e->effective_sort_timestamp) : kUnknownDay, out, cap);
}

// A signature of what the history draws from the index: order, days and
// unread markers. A change (an arrival, a removal, a read) means the shown
// rows must be redrawn even when the index itself stayed current.
inline uint32_t indexSignature(uint16_t total) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v) {
    for (int i = 0; i < 4; i++) {
      h ^= (v >> (8 * i)) & 0xFFu;
      h *= 16777619u;
    }
  };
  mix(total);
  for (uint16_t i = 0; i < total; i++) {
    const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
    if (e == nullptr) continue;
    mix(e->sequence);
    mix(thaiDay(e->effective_sort_timestamp));
    mix(e->flags & MessageStore::FLAG_UNREAD);
  }
  return h;
}

}  // namespace HistoryDates
